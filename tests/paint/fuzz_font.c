/* Fuzzing of the font parsers: seed fonts mutated (byte flips, interesting
 * values, truncation, copied ranges), mostly inside one table at a time,
 * then parsed and exercised: variation instances at random positions,
 * every glyph's outline and advance, metrics, colour glyphs, GSUB, and the
 * WOFF and WOFF2 decoders on wrapped inputs. Deterministic for a seed.
 *
 *   fuzz_font [iterations] [seed] [seconds] font...
 *
 * Run under the sanitizer build for memory errors; the program itself only
 * checks that nothing crashes or hangs. */
#include "paint/font.h"
#include "platform/platform.h"

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

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* A table's range in an sfnt (or the whole file when not an sfnt). */
static void pick_range(const uint8_t *d, size_t len, uint32_t *off, uint32_t *n)
{
    *off = 0;
    *n = (uint32_t)len;
    if (len < 12 || rnd(5) == 0)
        return;
    uint32_t dir = 0;
    if (be32(d) == 0x74746366 && len >= 16) /* ttcf */
        dir = be32(d + 12);
    if (dir > len - 12)
        return;
    uint32_t nt = ((uint32_t)d[dir + 4] << 8) | d[dir + 5];
    if (!nt || dir + 12 + nt * 16 > len)
        return;
    uint32_t t = rnd(nt);
    const uint8_t *r = d + dir + 12 + t * 16;
    uint32_t o = be32(r + 8), l = be32(r + 12);
    if (o < len && l && l <= len - o) {
        *off = o;
        *n = l;
    }
}

static void mutate(uint8_t *d, size_t *len)
{
    uint32_t off, n;
    pick_range(d, *len, &off, &n);
    if (!n)
        return;
    int count = 1 + (int)rnd(8);
    for (int k = 0; k < count; k++) {
        uint32_t at = off + rnd(n);
        switch (rnd(7)) {
        case 0: d[at] ^= (uint8_t)(1u << rnd(8)); break;
        case 1: d[at] = (uint8_t)rnd(256); break;
        case 2: d[at] = rnd(2) ? 0xFF : 0x00; break;
        case 3: /* an interesting 16-bit value */
            if (at + 1 < *len) {
                static const uint16_t v[] = {0, 1, 2, 0x7FFF, 0x8000, 0xFFFF, 0xFFFE, 0x4000, 0xC000, 0x0FFF};
                uint16_t x = v[rnd(sizeof v / sizeof v[0])];
                d[at] = (uint8_t)(x >> 8);
                d[at + 1] = (uint8_t)x;
            }
            break;
        case 4: /* copy a range over another */
        {
            uint32_t src = off + rnd(n), cnt = 1 + rnd(16);
            if (src + cnt <= *len && at + cnt <= *len)
                memmove(d + at, d + src, cnt);
            break;
        }
        case 5:
            if (rnd(8) == 0)
                *len = at; /* truncate */
            break;
        default: d[at] = (uint8_t)(d[at] + 1 - rnd(3)); break;
        }
    }
}

static void exercise(const uint8_t *data, size_t len)
{
    int nf = au_font_face_count(data, len);
    for (int fi = 0; fi < nf && fi < 2; fi++) {
        AuFontFace *f = au_font_face_new(data, len, fi);
        if (!f)
            continue;
        AuFontFace *faces[3] = {f, NULL, NULL};
        int naxes = au_font_axis_count(f);
        if (naxes) {
            AuFontVariation v[AU_FONT_MAX_AXES];
            for (int i = 0; i < naxes; i++) {
                const AuFontAxis *a = au_font_axis(f, i);
                v[i].tag = a->tag;
                v[i].value = a->min + (a->max - a->min) * (float)rnd(1001) / 1000.0f;
            }
            faces[1] = au_font_face_instance(f, v, (size_t)naxes);
            v[0].value = rnd(2) ? 1e9f : -1e9f;
            faces[2] = au_font_face_instance(f, v, 1);
            char nm[64];
            float c[AU_FONT_MAX_AXES];
            for (int i = 0; i < au_font_named_instance_count(f) && i < 4; i++)
                au_font_named_instance(f, i, nm, sizeof nm, c);
        }
        for (int k = 0; k < 3; k++) {
            AuFontFace *x = faces[k];
            if (!x)
                continue;
            AuFontMetrics m;
            au_font_metrics(x, 16, &m);
            unsigned ng = x->num_glyphs < 400 ? x->num_glyphs : 400;
            for (unsigned g = 0; g < ng; g++) {
                uint16_t id = (uint16_t)(x->num_glyphs < 400 ? g : rnd(x->num_glyphs));
                AuPath p;
                au_path_init(&p);
                au_font_glyph_path(x, id, 0.05f, 0.05f, 0, 0, &p);
                au_path_free(&p);
                (void)au_font_advance_units(x, id);
            }
            static const char *texts[] = {"Hello, world fi", "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD x",
                                          "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5 1\xEF\xB8\x8F\xE2\x83\xA3",
                                          "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"};
            AuGlyphRun r;
            au_glyph_run_init(&r);
            const char *t = texts[rnd(4)];
            au_shape_text(x, 16, t, strlen(t), &r);
            AuSurface *s = au_surface_new(48, 24);
            AuCanvas *c = au_canvas_new(s);
            au_draw_glyph_run(c, x, 6 + (float)rnd(40), &r, 2, 18, 0xFF000000);
            au_canvas_free(c);
            au_surface_free(s);
            au_glyph_run_free(&r);
        }
        au_font_face_free(faces[2]);
        au_font_face_free(faces[1]);
        au_font_face_free(f);
    }
    /* Clusters, GSUB and colour glyphs through a font list. */
    AuFontSet *set = au_fontset_new();
    if (au_fontset_add_data(set, data, len) > 0) {
        const char *fam = au_fontset_entry(set, 0, NULL, NULL);
        AuFontList *l = au_fontlist_new(set, &fam, 1, 100 + 100 * (int)rnd(9), rnd(2));
        static const char *seqs =
            "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD \xF0\x9F\x87\xAF\xF0\x9F\x87\xB5 1\xEF\xB8\x8F\xE2\x83\xA3 "
            "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7 \xF0\x9F\x8F\xB3\xEF\xB8\x8F"
            "\xE2\x80\x8D\xF0\x9F\x8C\x88 \xEE\x80\x80\xEE\x80\x81\xEE\x80\x82\xEE\x80\x83\xEE\x80\x84\xEE\x80\x85"
            "\xEE\x80\x86\xEE\x80\x87\xEE\x80\x88\xEE\x80\x89\xEE\x80\x8A\xEE\x80\x8B\xEE\x80\x8C\xEE\x80\x8D"
            "\xEE\x80\x8E\xEE\x80\x8F \xE2\x98\xBA\xEF\xB8\x8E Ab\xCC\x81";
        AuGlyphRun r;
        au_glyph_run_init(&r);
        au_shape_text_list(l, 20, seqs, strlen(seqs), &r);
        AuSurface *s = au_surface_new(64, 32);
        AuCanvas *c = au_canvas_new(s);
        au_draw_glyph_run(c, au_fontlist_primary(l), 8 + (float)rnd(40), &r, -(float)rnd(200), 24, 0xFF336699);
        au_canvas_free(c);
        au_surface_free(s);
        au_glyph_run_free(&r);
        au_fontlist_free(l);
    }
    au_fontset_free(set);
}

static double now(void)
{
    return (double)clock() / CLOCKS_PER_SEC;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: fuzz_font iterations seed seconds font...\n");
        return 2;
    }
    long iters = atol(argv[1]);
    rng = (uint32_t)atol(argv[2]) | 1;
    double limit = atof(argv[3]);
    int nseeds = argc - 4;
    char **seeds = calloc((size_t)nseeds, sizeof *seeds);
    size_t *lens = calloc((size_t)nseeds, sizeof *lens);
    for (int i = 0; i < nseeds; i++)
        if (!au_platform_read_file(argv[4 + i], &seeds[i], &lens[i])) {
            fprintf(stderr, "cannot read %s\n", argv[4 + i]);
            return 1;
        }
    au_glyph_cache_set_limit(256 * 1024);
    double t0 = now();
    long it;
    for (it = 0; it < iters && now() - t0 < limit; it++) {
        int s = (int)rnd((uint32_t)nseeds);
        size_t len = lens[s];
        uint8_t *d = malloc(len ? len : 1);
        memcpy(d, seeds[s], len);
        int rounds = 1 + (int)rnd(3);
        for (int r = 0; r < rounds; r++)
            mutate(d, &len);
        /* An exact-size copy, so reads past the end are caught. */
        uint8_t *e = malloc(len ? len : 1);
        memcpy(e, d, len);
        free(d);
        exercise(e, len);
        au_glyph_cache_clear();
        free(e);
    }
    printf("fuzz_font: %ld iterations in %.1f s over %d seed fonts\n", it, now() - t0, nseeds);
    for (int i = 0; i < nseeds; i++)
        au_free(seeds[i]);
    free(seeds);
    free(lens);
    return 0;
}
