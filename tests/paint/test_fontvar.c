/* Variable fonts: the engine's varied outlines, advances and metrics
 * against static instances made by fontTools' instancer.
 *
 *   test_fontvar DIR [--optional]
 *
 * DIR holds var-refs.txt (variable font, static instance, axis values per
 * line) and the fonts it names. tests/paint/fonts has small subsets that
 * are committed; tools/test-fonts.py refs writes full fonts into
 * build/test-fonts/refs. With --optional, a missing DIR skips (exit 77). */
#include "core/core.h"
#include "paint/font.h"
#include "platform/platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            exit(1); \
        } \
    } while (0)

static char *load(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    char *data;
    if (!au_platform_read_file(path, &data, len))
        return NULL;
    return data;
}

static double max_glyph_error(const AuFontFace *a, const AuFontFace *b, uint16_t g, int *ops_differ)
{
    AuPath pa, pb;
    au_path_init(&pa);
    au_path_init(&pb);
    au_font_glyph_path(a, g, 1, 1, 0, 0, &pa);
    au_font_glyph_path(b, g, 1, 1, 0, 0, &pb);
    double err = 0;
    *ops_differ = 0;
    if (pa.nops != pb.nops || pa.npts != pb.npts || (pa.nops && memcmp(pa.ops, pb.ops, pa.nops))) {
        *ops_differ = 1;
    } else {
        for (size_t i = 0; i < pa.npts; i++) {
            double d = fabs((double)pa.pts[i] - pb.pts[i]);
            if (d > err)
                err = d;
        }
    }
    au_path_free(&pa);
    au_path_free(&pb);
    return err;
}

static void compare(const char *dir, const char *varname, const char *instname, char *axes)
{
    size_t vlen, ilen;
    char *vdata = load(dir, varname, &vlen), *idata = load(dir, instname, &ilen);
    CHECK(vdata && idata);
    AuFontFace *var = au_font_face_new(vdata, vlen, 0);
    AuFontFace *ref = au_font_face_new(idata, ilen, 0);
    CHECK(var && ref);
    CHECK(au_font_axis_count(var) > 0);
    CHECK(au_font_axis_count(ref) == 0);
    AuFontVariation v[AU_FONT_MAX_AXES];
    size_t nv = 0;
    for (char *tok = strtok(axes, ","); tok && nv < AU_FONT_MAX_AXES; tok = strtok(NULL, ",")) {
        CHECK(strlen(tok) > 5 && tok[4] == '=');
        v[nv].tag = AU_FONT_TAG(tok[0], tok[1], tok[2], tok[3]);
        v[nv].value = (float)atof(tok + 5);
        nv++;
    }
    AuFontFace *inst = au_font_face_instance(var, v, nv);
    CHECK(inst->varied || nv == 0);
    CHECK(inst->num_glyphs == ref->num_glyphs);
    double worst = 0;
    int worst_adv = 0, bad_ops = 0;
    for (unsigned g = 0; g < inst->num_glyphs; g++) {
        int od;
        double e = max_glyph_error(inst, ref, (uint16_t)g, &od);
        if (od) {
            fprintf(stderr, "%s %s: glyph %u: outline structure differs\n", varname, instname, g);
            bad_ops++;
        }
        if (e > worst)
            worst = e;
        int ia = au_font_advance_units(inst, (uint16_t)g), ra = au_font_advance_units(ref, (uint16_t)g);
        int da = abs(ia - ra);
        /* Open Sans's HVAR gives .notdef advance deltas its gvar does not
         * have; the instancer takes advances from gvar's phantom points,
         * the engine (as browsers do) from HVAR. */
        if (g == 0 && au_ieq(varname, 8, "opensans", 8))
            da = 0;
        if (da > 1)
            fprintf(stderr, "%s %s: glyph %u: advance %d, reference %d\n", varname, instname, g, ia, ra);
        if (da > worst_adv)
            worst_adv = da;
    }
    int dm = 0;
    int pairs[][2] = {{inst->ascender, ref->ascender},       {inst->descender, ref->descender},
                      {inst->line_gap, ref->line_gap},       {inst->x_height, ref->x_height},
                      {inst->cap_height, ref->cap_height},   {inst->strike_pos, ref->strike_pos},
                      {inst->underline_pos, ref->underline_pos}};
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++)
        if (abs(pairs[i][0] - pairs[i][1]) > dm)
            dm = abs(pairs[i][0] - pairs[i][1]);
    printf("%-22s %-36s glyphs %4u  max outline error %.3f units, max advance error %d, max metric error %d, "
           "weight %d (ref %d)\n",
           varname, instname, (unsigned)inst->num_glyphs, worst, worst_adv, dm, inst->weight, ref->weight);
    /* The instancer rounds coordinates, offsets and advances to whole
     * units: allow one unit (plus float noise). */
    if (worst > 1.01 || worst_adv > 1 || dm > 1 || bad_ops || abs((int)inst->weight - (int)ref->weight) > 1)
        failures++;
    au_font_face_free(inst);
    au_font_face_free(var);
    au_font_face_free(ref);
    au_free(vdata);
    au_free(idata);
}

/* Matching: a variable face registered once serves a weight range with
 * instances, not synthesis; layered sets and unicode-range. */
static void test_matching(const char *dir)
{
    size_t len;
    char *data = load(dir, "intervar.ttf", &len);
    CHECK(data);
    AuFontSet *s = au_fontset_new();
    CHECK(au_fontset_add_data(s, data, len) == 1);
    const char *fam = au_fontset_entry(s, 0, NULL, NULL);
    printf("registered family \"%s\"\n", fam);
    int weights[] = {100, 300, 400, 650, 700, 900};
    AuFontFace *seen[6];
    for (int i = 0; i < 6; i++) {
        AuFontFace *f = au_fontset_match(s, fam, weights[i], false);
        CHECK(f);
        CHECK(f->weight == weights[i]);
        CHECK(!(f->synth & AU_FONT_SYNTH_BOLD));
        CHECK(f->varied == (weights[i] != 400));
        seen[i] = f;
        printf("  weight %d: subfamily \"%s\"\n", weights[i], f->subfamily);
    }
    /* The same request gives the same (cached) instance. */
    CHECK(au_fontset_match(s, fam, 700, false) == seen[4]);
    CHECK(!strcmp(seen[4]->subfamily, "Bold"));
    /* Weights outside the axis clamp to it. */
    AuFontFace *thin = au_fontset_match(s, fam, 50, false);
    CHECK(thin->weight == 100);
    /* No ital or slnt axis: italic is synthesized on the instance. */
    AuFontFace *it = au_fontset_match(s, fam, 700, true);
    CHECK(it->italic && (it->synth & AU_FONT_SYNTH_ITALIC) && !(it->synth & AU_FONT_SYNTH_BOLD) && it->weight == 700);
    /* Optical size follows the font size when asked. */
    AuFontStyle st = {400, false, 0, 32, NULL, 0};
    AuFontFace *big = au_fontset_match_style(s, fam, &st);
    int opsz = au_font_axis_find(big, AU_FONT_TAG('o', 'p', 's', 'z'));
    CHECK(opsz >= 0 && big->coords[opsz] == 16384);
    /* font-variation-settings override the weight. */
    AuFontVariation fv = {AU_FONT_TAG('w', 'g', 'h', 't'), 850};
    AuFontStyle st2 = {400, false, 0, 0, &fv, 1};
    CHECK(au_fontset_match_style(s, fam, &st2)->weight == 850);
    /* Advances vary with the weight. */
    uint16_t g = au_font_glyph(seen[0], 'H');
    CHECK(au_font_advance_units(seen[0], g) < au_font_advance_units(seen[5], g));

    /* A layered set: a web face under a page's family name, limited to
     * digits by unicode-range, over the platform set. */
    AuFontSet *doc = au_fontset_new_layered(s);
    char *copy = au_alloc(len);
    memcpy(copy, data, len);
    uint32_t range[2] = {'0', '9'};
    CHECK(au_fontset_add_web(doc, copy, len, "PageFont", 100, 900, false, range, 1));
    CHECK(au_fontset_has_family(doc, "PageFont", 8));
    CHECK(au_fontset_has_family(doc, fam, strlen(fam)));
    CHECK(!au_fontset_has_family(s, "PageFont", 8));
    const char *names[] = {"PageFont", fam};
    AuFontList *l = au_fontlist_new(doc, names, 2, 700, false);
    AuFontFace *digit = au_fontlist_face_for(l, '1', false);
    AuFontFace *letter = au_fontlist_face_for(l, 'H', false);
    CHECK(digit && letter && digit != letter);
    CHECK(digit->weight == 700 && letter->weight == 700);
    au_fontlist_free(l);
    /* Data that is not a font is refused and freed. */
    char *junk = au_alloc(64);
    memset(junk, 0, 64);
    CHECK(!au_fontset_add_web(doc, junk, 64, "Junk", 400, 400, false, NULL, 0));
    au_fontset_free(doc);
    au_fontset_free(s);
    au_free(data);
}

static double now_ms(void)
{
    return (double)clock() * 1000.0 / CLOCKS_PER_SEC;
}

/* Outline extraction and a cold-cache draw of every glyph: the default
 * (static) instance against a varied one, and the static instance file
 * fontTools made at the same position. */
static void bench(const char *dir)
{
    size_t vl, sl;
    char *vd = load(dir, "intervar.ttf", &vl), *sd = load(dir, "intervar@opsz14-wght700.ttf", &sl);
    CHECK(vd && sd);
    AuFontFace *def = au_font_face_new(vd, vl, 0), *stat = au_font_face_new(sd, sl, 0);
    AuFontVariation v[2] = {{AU_FONT_TAG('w', 'g', 'h', 't'), 700}, {AU_FONT_TAG('o', 'p', 's', 'z'), 14}};
    AuFontFace *var = au_font_face_instance(def, v, 2);
    CHECK(def && stat && var);
    AuFontFace *faces[3] = {def, stat, var};
    const char *names[3] = {"default instance", "static file (fontTools)", "varied (wght 700, opsz 14)"};
    AuSurface *s = au_surface_new(64, 64);
    AuCanvas *c = au_canvas_new(s);
    for (int k = 0; k < 3; k++) {
        AuFontFace *f = faces[k];
        int reps = 400;
        double t0 = now_ms();
        for (int r = 0; r < reps; r++)
            for (unsigned g = 0; g < f->num_glyphs; g++) {
                AuPath p;
                au_path_init(&p);
                au_font_glyph_path(f, (uint16_t)g, 0.016f, 0.016f, 0, 0, &p);
                au_path_free(&p);
            }
        double path = (now_ms() - t0) / (reps * (double)f->num_glyphs);
        AuGlyphRun run;
        au_glyph_run_init(&run);
        au_shape_text(f, 16, "AQRSaegxy&@", 11, &run);
        t0 = now_ms();
        for (int r = 0; r < reps; r++) {
            au_glyph_cache_clear();
            au_draw_glyph_run(c, f, 16, &run, 2, 40, 0xFF000000);
        }
        double draw = (now_ms() - t0) / (reps * (double)run.n);
        au_glyph_run_free(&run);
        printf("%-28s outline %.4f ms per glyph; 16 px rasterize + draw %.4f ms per glyph\n", names[k], path, draw);
    }
    au_canvas_free(c);
    au_surface_free(s);
    au_font_face_free(var);
    au_font_face_free(stat);
    au_font_face_free(def);
    au_free(vd);
    au_free(sd);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    const char *dir = argv[1];
    if (argc > 2 && !strcmp(argv[2], "--bench")) {
        bench(dir);
        return 0;
    }
    bool optional = argc > 2 && !strcmp(argv[2], "--optional");
    size_t len;
    char *refs = load(dir, "var-refs.txt", &len);
    if (!refs) {
        if (optional) {
            printf("no %s/var-refs.txt: run tools/test-fonts.py fetch and refs to enable this test\n", dir);
            return 77;
        }
        fprintf(stderr, "cannot read %s/var-refs.txt\n", dir);
        return 1;
    }
    char *text = au_alloc(len + 1);
    memcpy(text, refs, len);
    text[len] = 0;
    int lines = 0;
    for (char *line = text; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (line[0] && line[0] != '#') {
            char *a = line, *b = strchr(a, '\t');
            CHECK(b);
            *b++ = 0;
            char *c = strchr(b, '\t');
            CHECK(c);
            *c++ = 0;
            compare(dir, a, b, c);
            lines++;
        }
        line = nl ? nl + 1 : NULL;
    }
    CHECK(lines > 0);
    au_free(text);
    au_free(refs);
    if (!optional)
        test_matching(dir);
    if (failures) {
        fprintf(stderr, "%d instance(s) differ from the reference beyond tolerance\n", failures);
        return 1;
    }
    return 0;
}
