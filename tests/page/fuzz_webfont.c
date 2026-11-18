/* Fuzzing of @font-face loading: random and mutated @font-face rules
 * (src lists, formats, technologies, weights, styles, unicode-range text,
 * font-display) with a clock moved across the block periods,
 * random text, and a loader that answers in random order with the sample
 * WOFF2 font, mutated copies of it, garbage, empty bodies or failures,
 * with documents replaced and views freed while fonts load.
 *
 *   fuzz_webfont [iterations] [seed] [seconds]
 *
 * Run under the sanitizer build for memory errors. */
#include "../layout/ahem.h"
#include "page/page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t rng;

static uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return n ? rng % n : 0;
}

typedef struct Req {
    uint32_t id;
    bool live;
} Req;

typedef struct Loader {
    Req req[256];
    int n;
    uint32_t next;
} Loader;

static uint32_t req_fn(void *ctx, AuDocView *dv, const char *url, AuResourceKind kind)
{
    (void)dv, (void)url, (void)kind;
    Loader *l = ctx;
    if (l->n == 256 || rnd(20) == 0)
        return 0;
    l->req[l->n].id = ++l->next;
    l->req[l->n].live = true;
    l->n++;
    return l->next;
}

static char *font;
static size_t font_len;

static void answer(Loader *l, AuDocView *dv)
{
    if (!l->n)
        return;
    int i = (int)rnd((uint32_t)l->n);
    if (!l->req[i].live)
        return;
    l->req[i].live = false;
    uint32_t id = l->req[i].id;
    switch (rnd(6)) {
    case 0: au_docview_resource_done(dv, id, false); return;
    case 1: au_docview_resource_done(dv, id, true); return;
    case 2: {
        char junk[64];
        for (size_t k = 0; k < sizeof junk; k++)
            junk[k] = (char)rnd(256);
        if (rnd(2))
            memcpy(junk, rnd(2) ? "wOF2" : "wOFF", 4);
        au_docview_resource_data(dv, id, junk, 1 + rnd(sizeof junk));
        au_docview_resource_done(dv, id, true);
        return;
    }
    case 3: { /* the font mutated, in random chunks */
        char *m = malloc(font_len);
        memcpy(m, font, font_len);
        for (int k = 0; k < 1 + (int)rnd(6); k++) {
            size_t at = rnd(2) ? rnd(600) : rnd((uint32_t)font_len);
            m[at % font_len] ^= (char)(1 << rnd(8));
        }
        size_t len = rnd(8) ? font_len : rnd((uint32_t)font_len);
        size_t at = 0;
        while (at < len) {
            size_t k = 1 + rnd(9000);
            if (k > len - at)
                k = len - at;
            au_docview_resource_data(dv, id, m + at, k);
            at += k;
        }
        au_docview_resource_done(dv, id, true);
        free(m);
        return;
    }
    default:
        au_docview_resource_data(dv, id, font, font_len);
        au_docview_resource_done(dv, id, true);
        return;
    }
}

static const char *const srcs[] = {
    "url(a.woff2)", "url(a.woff2) format('woff2')", "url(b.woff) format(woff)", "url(c.ttf) format('truetype')",
    "url(d.otf) format(opentype)", "url(e.woff2) format('woff2') tech(variations)", "url(f.woff2) tech(color-svg)",
    "local('Ahem')", "local(Ahem)", "local('Nope')", "url()", "url(x) format()", "url(y) format(1)", "local()",
    "url(data:font/woff2;base64,AAAA)", "url(z) format('woff2', 'woff')", "url(q) tech(incremental)"};
static const char *const ranges[] = {"U+0-7F", "U+0400-045F", "U+30-39, U+41-5A", "U+4??", "U+1F600-1F64F",
                                     "U+0-10FFFF", "U+110000", "U+7F-0", "u+00?\?-01FF", "U+", "U+zz",
                                     "U+0041, U+0042-", "U+0000000-1", "U+30-39,,U+41"};
static const char *const weights[] = {"100 900", "bold", "normal", "300", "900 100", "auto", "1000 1", "0", "x"};
static const char *const displays[] = {"auto", "block", "swap", "fallback", "optional", "AUTO", "x", "1", "swap swap", ""};
static const char *const texts[] = {"Hello", "0123456789", "Привет", "😀 x", "", " ", "abc\xCC\x81", "\xFF\xFE",
                                    "日本語"};

static void gen(char *out, size_t cap)
{
    size_t n = 0;
    n += (size_t)snprintf(out + n, cap - n, "<!DOCTYPE html><style>");
    int faces = 1 + (int)rnd(6);
    for (int i = 0; i < faces && n < cap - 600; i++) {
        n += (size_t)snprintf(out + n, cap - n, "@font-face{font-family:%s;src:", rnd(2) ? "F" : "'G H'");
        int ns = 1 + (int)rnd(4);
        for (int k = 0; k < ns; k++)
            n += (size_t)snprintf(out + n, cap - n, "%s%s", k ? "," : "", srcs[rnd(sizeof srcs / sizeof srcs[0])]);
        if (rnd(2))
            n += (size_t)snprintf(out + n, cap - n, ";unicode-range:%s", ranges[rnd(sizeof ranges / sizeof ranges[0])]);
        if (rnd(2))
            n += (size_t)snprintf(out + n, cap - n, ";font-weight:%s", weights[rnd(sizeof weights / sizeof weights[0])]);
        if (rnd(2))
            n += (size_t)snprintf(out + n, cap - n, ";font-display:%s", displays[rnd(sizeof displays / sizeof displays[0])]);
        if (rnd(2))
            n += (size_t)snprintf(out + n, cap - n, ";font-style:%s", rnd(2) ? "italic" : "oblique 10deg");
        n += (size_t)snprintf(out + n, cap - n, "%s", rnd(8) ? "}" : "/*");
        if (rnd(10) == 0)
            n += (size_t)snprintf(out + n, cap - n, "@media print{@font-face{font-family:F;src:url(m)}}");
        if (rnd(10) == 0)
            n += (size_t)snprintf(out + n, cap - n, "p{@font-face{font-family:F;src:url(n)}}");
    }
    n += (size_t)snprintf(out + n, cap - n, "</style><body style='font-family:F,\"G H\",serif'>");
    int paras = 1 + (int)rnd(5);
    for (int i = 0; i < paras && n < cap - 200; i++)
        n += (size_t)snprintf(out + n, cap - n, "<p style='font-weight:%d;font-style:%s'>%s</p>", 100 * (1 + (int)rnd(9)),
                              rnd(3) ? "normal" : "italic", texts[rnd(sizeof texts / sizeof texts[0])]);
    /* Byte damage. */
    for (int k = 0; k < (int)rnd(3); k++)
        if (n)
            out[rnd((uint32_t)n)] = "{}();:'\"/*@,-+?"[rnd(15)];
    out[n] = 0;
}

/* The view's clock: moves forward in random steps, across block periods. */
static double fake_ms;

static double clock_fn(void *ctx, AuDocView *dv)
{
    (void)ctx;
    (void)dv;
    return fake_ms;
}

static double now(void)
{
    return (double)clock() / CLOCKS_PER_SEC;
}

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 500;
    rng = (argc > 2 ? (uint32_t)atol(argv[2]) : 1) | 1;
    double limit = argc > 3 ? atof(argv[3]) : 60;
    char path[1024];
    snprintf(path, sizeof path, "%s/sample-inter.woff2", AURA_PAGE_SAMPLES_DIR);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    font_len = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    font = malloc(font_len);
    font_len = fread(font, 1, font_len, fp);
    fclose(fp);
    static uint8_t fontdata[16384];
    AuFontSet *fonts = ahem_fontset(fontdata);
    double t0 = now();
    long it;
    for (it = 0; it < iters && now() - t0 < limit; it++) {
        Loader l;
        memset(&l, 0, sizeof l);
        AuLoader li = {&l, req_fn, NULL, NULL, NULL, NULL};
        AuDocViewCallbacks cb;
        memset(&cb, 0, sizeof cb);
        cb.now_ms = clock_fn;
        fake_ms = rnd(1000);
        AuDocView *dv = au_docview_new(fonts, &li, rnd(4) ? &cb : NULL);
        au_docview_set_viewport(dv, 200 + (int)rnd(600), 300);
        int docs = 1 + (int)rnd(2);
        for (int d = 0; d < docs; d++) {
            static char html[8192];
            gen(html, sizeof html);
            au_docview_load(dv, html, strlen(html), "http://example.com/p");
            for (int step = 0; step < 30; step++) {
                au_docview_update(dv);
                if (rnd(3) == 0) {
                    AuSurface *s = au_surface_new(64, 64);
                    AuCanvas *c = au_canvas_new(s);
                    au_docview_paint(dv, c, (AuRect){0, 0, 64, 64}, 0, 0);
                    au_canvas_free(c);
                    au_surface_free(s);
                }
                answer(&l, dv);
                if (rnd(2)) {
                    fake_ms += rnd(4) == 0 ? rnd(5000) : rnd(150);
                    au_docview_animate(dv, (uint32_t)fake_ms);
                }
                if (rnd(40) == 0)
                    break; /* leave fonts loading */
            }
        }
        au_docview_free(dv);
    }
    printf("fuzz_webfont: %ld iterations in %.1f s\n", it, now() - t0);
    au_fontset_free(fonts);
    free(font);
    return 0;
}
