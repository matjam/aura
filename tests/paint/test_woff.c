/* WOFF and WOFF2 decoding: the decoded fonts against the sfnt files they
 * were made from (fontTools for the committed ones, Google's woff2
 * encoder for the upstream Inter file when tools/test-fonts.py fetch has
 * run), then truncated, corrupt and refused inputs.
 *
 *   test_woff FONTS_DIR [TEST_FONTS_DIR] [--bench FILE] */
#include "core/core.h"
#include "paint/font.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            exit(1); \
        } \
    } while (0)

#define MAX_OUT (32u * 1024 * 1024)

static char *load(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    char *data;
    if (!au_platform_read_file(path, &data, len))
        return NULL;
    return data;
}

static bool same_path(const AuPath *a, const AuPath *b)
{
    if (a->nops != b->nops || a->npts != b->npts)
        return false;
    return (!a->nops || !memcmp(a->ops, b->ops, a->nops)) && (!a->npts || !memcmp(a->pts, b->pts, a->npts * sizeof *a->pts));
}

/* Every glyph's outline and advance, at the default and at one varied
 * position. */
static void compare_faces(AuFontFace *a, AuFontFace *b, const char *what)
{
    CHECK(a && b);
    CHECK(a->num_glyphs == b->num_glyphs);
    CHECK(a->units_per_em == b->units_per_em);
    CHECK(au_font_axis_count(a) == au_font_axis_count(b));
    AuFontFace *va = NULL, *vb = NULL;
    if (au_font_axis_count(a)) {
        AuFontVariation v = {AU_FONT_TAG('w', 'g', 'h', 't'), 730};
        va = au_font_face_instance(a, &v, 1);
        vb = au_font_face_instance(b, &v, 1);
        CHECK(va && vb);
    }
    AuFontFace *fa[2] = {a, va}, *fb[2] = {b, vb};
    for (int k = 0; k < 2 && fa[k]; k++)
        for (unsigned g = 0; g < a->num_glyphs; g++) {
            AuPath p, q;
            au_path_init(&p);
            au_path_init(&q);
            au_font_glyph_path(fa[k], (uint16_t)g, 0.1f, 0.1f, 0, 0, &p);
            au_font_glyph_path(fb[k], (uint16_t)g, 0.1f, 0.1f, 0, 0, &q);
            if (!same_path(&p, &q) || au_font_advance_units(fa[k], (uint16_t)g) != au_font_advance_units(fb[k], (uint16_t)g)) {
                fprintf(stderr, "%s: glyph %u differs (%s)\n", what, g, k ? "varied" : "default");
                exit(1);
            }
            au_path_free(&p);
            au_path_free(&q);
        }
    au_font_face_free(va);
    au_font_face_free(vb);
}

static uint8_t *decode_ok(const char *data, size_t len, size_t *out_len)
{
    uint8_t *out;
    AuFontStatus st = au_font_decode(data, len, MAX_OUT, &out, out_len);
    CHECK(st == AU_FONT_OK);
    CHECK(out && *out_len);
    return out;
}

static void check_pair(const char *dir, const char *container, const char *ttf, int index)
{
    size_t clen, tlen, olen;
    char *c = load(dir, container, &clen), *t = load(dir, ttf, &tlen);
    CHECK(c && t);
    uint8_t *o = decode_ok(c, clen, &olen);
    AuFontFace *a = au_font_face_new(o, olen, index), *b = au_font_face_new(t, tlen, 0);
    char what[256];
    snprintf(what, sizeof what, "%s[%d] vs %s", container, index, ttf);
    compare_faces(a, b, what);
    printf("ok: %s (%zu bytes -> %zu, %u glyphs)\n", what, clen, olen, a->num_glyphs);
    au_font_face_free(a);
    au_font_face_free(b);
    au_free(o);
    au_free(c);
    au_free(t);
}

/* Any outcome but a crash; an accepted output must load or be refused
 * cleanly. */
static int try_decode(const uint8_t *d, size_t len, size_t max_out)
{
    uint8_t *e = malloc(len ? len : 1); /* exact size, so overreads trip ASan */
    memcpy(e, d, len);
    uint8_t *o;
    size_t olen;
    AuFontStatus st = au_font_decode(e, len, max_out, &o, &olen);
    if (st == AU_FONT_OK) {
        CHECK(olen <= max_out);
        int n = au_font_face_count(o, olen);
        for (int i = 0; i < n && i < 2; i++) {
            AuFontFace *f = au_font_face_new(o, olen, i);
            if (f) {
                AuPath p;
                au_path_init(&p);
                for (unsigned g = 0; g < f->num_glyphs && g < 64; g++)
                    au_font_glyph_path(f, (uint16_t)g, 0.1f, 0.1f, 0, 0, &p);
                au_path_free(&p);
                au_font_face_free(f);
            }
        }
        au_free(o);
    } else {
        CHECK(!o && !olen);
    }
    free(e);
    return (int)st;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void check_corrupt(const char *dir, const char *container)
{
    size_t len;
    char *c = load(dir, container, &len);
    CHECK(c);
    const uint8_t *d = (const uint8_t *)c;
    /* Every truncation of the headers and a sample of the rest: never
     * accepted (the declared length no longer matches). */
    for (size_t n = 0; n < len; n += n < 400 ? 1 : 13)
        CHECK(try_decode(d, n, MAX_OUT) != AU_FONT_OK);
    /* The declared length kept, the data cut short. */
    uint8_t *m = malloc(len);
    for (size_t n = 48; n < len; n += 97) {
        memcpy(m, d, n);
        put32(m + 8, (uint32_t)n);
        CHECK(try_decode(m, n, MAX_OUT) != AU_FONT_OK);
    }
    /* An output limit below the font's size. */
    size_t olen;
    uint8_t *o = decode_ok(c, len, &olen);
    au_free(o);
    CHECK(try_decode(d, len, olen - 1) == AU_FONT_BAD);
    CHECK(try_decode(d, len, olen) == AU_FONT_OK);
    /* Byte changes anywhere: no crash, whatever the result. */
    uint32_t rng = 12345;
    int accepted = 0;
    for (int it = 0; it < 3000; it++) {
        memcpy(m, d, len);
        for (int k = 0; k < 1 + it % 4; k++) {
            rng = rng * 1103515245u + 12345u;
            size_t at = (rng >> 8) % (it % 3 ? 400 < len ? 400 : len : len);
            m[at] ^= (uint8_t)(1u << ((rng >> 4) & 7));
        }
        accepted += try_decode(m, len, MAX_OUT) == AU_FONT_OK;
    }
    printf("ok: %s corrupt and truncated (%d of 3000 changed inputs still decode)\n", container, accepted);
    free(m);
    au_free(c);
}

static void check_refused(const char *dir)
{
    size_t len;
    char *c = load(dir, "intervar.woff", &len);
    CHECK(c);
    uint8_t *m = malloc(len);
    /* CFF flavour. */
    memcpy(m, c, len);
    memcpy(m + 4, "OTTO", 4);
    CHECK(try_decode(m, len, MAX_OUT) == AU_FONT_UNSUPPORTED);
    /* A CFF table in a TrueType-flavoured file. */
    memcpy(m, c, len);
    memcpy(m + 44, "CFF ", 4);
    CHECK(try_decode(m, len, MAX_OUT) == AU_FONT_UNSUPPORTED);
    /* A duplicate table. */
    memcpy(m, c, len);
    memcpy(m + 64, m + 44, 4);
    CHECK(try_decode(m, len, MAX_OUT) == AU_FONT_BAD);
    /* An original length smaller than the compressed one. */
    memcpy(m, c, len);
    put32(m + 44 + 12, 1);
    CHECK(try_decode(m, len, MAX_OUT) == AU_FONT_BAD);
    /* An original length larger than the zlib data gives. */
    memcpy(m, c, len);
    for (int i = 0; i < 4; i++) {
        uint8_t *e = m + 44 + i * 20;
        uint32_t cl = ((uint32_t)e[8] << 24) | ((uint32_t)e[9] << 16) | ((uint32_t)e[10] << 8) | e[11];
        uint32_t ol = ((uint32_t)e[12] << 24) | ((uint32_t)e[13] << 16) | ((uint32_t)e[14] << 8) | e[15];
        if (cl < ol) {
            put32(e + 12, ol + 1);
            break;
        }
    }
    CHECK(try_decode(m, len, MAX_OUT) == AU_FONT_BAD);
    free(m);
    au_free(c);

    c = load(dir, "intervar.woff2", &len);
    CHECK(c);
    m = malloc(len);
    memcpy(m, c, len);
    memcpy(m + 4, "OTTO", 4);
    CHECK(try_decode(m, len, MAX_OUT) == AU_FONT_UNSUPPORTED);
    free(m);
    au_free(c);

    static const uint8_t otf[] = {'O', 'T', 'T', 'O', 0, 0, 0, 0};
    CHECK(au_font_sniff(otf, sizeof otf) == AU_FONT_FORMAT_CFF);
    CHECK(try_decode(otf, sizeof otf, MAX_OUT) == AU_FONT_UNSUPPORTED);
    CHECK(try_decode((const uint8_t *)"junk", 4, MAX_OUT) == AU_FONT_BAD);
    printf("ok: refused inputs\n");
}

static double now_ms(void)
{
    return (double)clock() * 1000.0 / CLOCKS_PER_SEC;
}

static void bench(const char *path)
{
    char *d;
    size_t len;
    if (!au_platform_read_file(path, &d, &len)) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(1);
    }
    int reps = 20;
    size_t olen = 0;
    double t0 = now_ms();
    for (int i = 0; i < reps; i++) {
        uint8_t *o;
        CHECK(au_font_decode(d, len, MAX_OUT, &o, &olen) == AU_FONT_OK);
        au_free(o);
    }
    printf("decode %s: %zu bytes -> %zu, %.2f ms\n", path, len, olen, (now_ms() - t0) / reps);
    au_free(d);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_woff FONTS_DIR [TEST_FONTS_DIR] [--bench FILE]\n");
        return 2;
    }
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--bench")) {
            bench(argv[i + 1]);
            return 0;
        }
    const char *dir = argv[1];
    check_pair(dir, "intervar.woff", "intervar.ttf", 0);
    check_pair(dir, "opensansvar.woff", "opensansvar.ttf", 0);
    check_pair(dir, "intervar.woff2", "intervar.ttf", 0);
    check_pair(dir, "opensansvar-hmtx.woff2", "opensansvar.ttf", 0);
    {
        size_t len, olen;
        char *c = load(dir, "collection.woff2", &len);
        CHECK(c);
        uint8_t *o = decode_ok(c, len, &olen);
        CHECK(au_font_face_count(o, olen) == 2);
        au_free(o);
        au_free(c);
    }
    check_pair(dir, "collection.woff2", "intervar.ttf", 0);
    check_pair(dir, "collection.woff2", "opensansvar.ttf", 1);
    /* The upstream file, made by Google's encoder. */
    if (argc > 2) {
        size_t n;
        char *probe = load(argv[2], "InterVariable.woff2", &n);
        if (probe) {
            au_free(probe);
            check_pair(argv[2], "InterVariable.woff2", "InterVariable.ttf", 0);
        } else {
            printf("skip: %s/InterVariable.woff2 not fetched\n", argv[2]);
        }
    }
    check_refused(dir);
    check_corrupt(dir, "intervar.woff");
    check_corrupt(dir, "intervar.woff2");
    check_corrupt(dir, "opensansvar-hmtx.woff2");
    check_corrupt(dir, "collection.woff2");
    printf("woff: all passed\n");
    return 0;
}
