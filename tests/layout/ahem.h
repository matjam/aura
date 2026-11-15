/* An Ahem-like TrueType font built in memory for geometry tests: every
 * printable ASCII character is a full-em square (ascent 0.8 em, descent
 * 0.2 em), space and no-break space are empty glyphs one em wide. Family
 * name "Ahem". */
#ifndef AURA_TEST_AHEM_H
#define AURA_TEST_AHEM_H

#include "paint/font.h"

#include <string.h>

typedef struct {
    uint8_t d[16384];
    size_t n;
} AhemW;

static void aw8(AhemW *w, unsigned v) { w->d[w->n++] = (uint8_t)v; }
static void aw16(AhemW *w, unsigned v) { aw8(w, v >> 8); aw8(w, v & 255); }
static void aw32(AhemW *w, uint32_t v) { aw16(w, v >> 16); aw16(w, v & 0xFFFF); }
static void apad(AhemW *w) { while (w->n & 3) aw8(w, 0); }

#define AHEM_GLYPHS 96 /* 0 notdef, 1..94 squares for 0x21..0x7E, 95 space */

/* Builds the font into out (at least 16 KB); returns its length. */
static size_t ahem_build(uint8_t *out)
{
    static AhemW w;
    memset(&w, 0, sizeof w);
    struct { const char *tag; size_t off, len; } t[9];
    int nt = 0;
    w.n = 12 + 16 * 9;
#define AB(name) do { apad(&w); t[nt].tag = name; t[nt].off = w.n; } while (0)
#define AE() do { t[nt].len = w.n - t[nt].off; nt++; } while (0)
    AB("cmap");
    aw16(&w, 0); aw16(&w, 1);
    aw16(&w, 3); aw16(&w, 1); aw32(&w, 12);
    /* format 4, 4 segments: [20], [21..7E], [A0], [FFFF] */
    int segs = 4;
    aw16(&w, 4); aw16(&w, (unsigned)(16 + segs * 8)); aw16(&w, 0);
    aw16(&w, (unsigned)(segs * 2)); aw16(&w, 8); aw16(&w, 2); aw16(&w, 0);
    aw16(&w, 0x20); aw16(&w, 0x7E); aw16(&w, 0xA0); aw16(&w, 0xFFFF); /* end */
    aw16(&w, 0);
    aw16(&w, 0x20); aw16(&w, 0x21); aw16(&w, 0xA0); aw16(&w, 0xFFFF); /* start */
    aw16(&w, (unsigned)(95 - 0x20) & 0xFFFF); aw16(&w, (unsigned)(1 - 0x21) & 0xFFFF);
    aw16(&w, (unsigned)(95 - 0xA0) & 0xFFFF); aw16(&w, 1);
    aw16(&w, 0); aw16(&w, 0); aw16(&w, 0); aw16(&w, 0);
    AE();
    AB("glyf");
    size_t loca[AHEM_GLYPHS + 1];
    loca[0] = 0; /* glyph 0 empty */
    for (int g = 1; g <= 94; g++) {
        loca[g] = w.n - t[nt].off;
        /* a square 0..1000 x -200..800 */
        aw16(&w, 1); aw16(&w, 0); aw16(&w, (unsigned)-200 & 0xFFFF); aw16(&w, 1000); aw16(&w, 800);
        aw16(&w, 3); aw16(&w, 0);
        for (int i = 0; i < 4; i++)
            aw8(&w, 1);
        aw16(&w, 0); aw16(&w, 1000); aw16(&w, 0); aw16(&w, (unsigned)-1000 & 0xFFFF);
        aw16(&w, (unsigned)-200 & 0xFFFF); aw16(&w, 0); aw16(&w, 1000); aw16(&w, 0);
        apad(&w);
    }
    loca[95] = w.n - t[nt].off; /* space: empty */
    loca[96] = loca[95];
    AE();
    AB("head");
    aw32(&w, 0x00010000); aw32(&w, 0); aw32(&w, 0); aw32(&w, 0x5F0F3CF5);
    aw16(&w, 0); aw16(&w, 1000);
    for (int i = 0; i < 4; i++) aw32(&w, 0);
    aw16(&w, 0); aw16(&w, (unsigned)-200 & 0xFFFF); aw16(&w, 1000); aw16(&w, 800);
    aw16(&w, 0); aw16(&w, 8); aw16(&w, 2);
    aw16(&w, 1); aw16(&w, 0); /* long loca */
    AE();
    AB("hhea");
    aw32(&w, 0x00010000); aw16(&w, 800); aw16(&w, (unsigned)-200 & 0xFFFF); aw16(&w, 0);
    for (int i = 0; i < 12; i++) aw16(&w, 0);
    aw16(&w, AHEM_GLYPHS);
    AE();
    AB("hmtx");
    for (int g = 0; g < AHEM_GLYPHS; g++) {
        aw16(&w, 1000);
        aw16(&w, 0);
    }
    AE();
    AB("loca");
    for (int g = 0; g <= AHEM_GLYPHS; g++)
        aw32(&w, (uint32_t)loca[g]);
    AE();
    AB("maxp");
    aw32(&w, 0x00005000); aw16(&w, AHEM_GLYPHS);
    AE();
    AB("name");
    aw16(&w, 0); aw16(&w, 1); aw16(&w, 6 + 12);
    aw16(&w, 3); aw16(&w, 1); aw16(&w, 0x409); aw16(&w, 1); aw16(&w, 8); aw16(&w, 0);
    const char *nm = "Ahem";
    for (const char *c = nm; *c; c++) aw16(&w, (unsigned char)*c);
    AE();
    AB("post");
    aw32(&w, 0x00030000); aw32(&w, 0); aw16(&w, (unsigned)-100 & 0xFFFF); aw16(&w, 50);
    for (int i = 0; i < 5; i++) aw32(&w, 0);
    AE();
    apad(&w);
    size_t total = w.n;
    w.n = 0;
    aw32(&w, 0x00010000); aw16(&w, (unsigned)nt); aw16(&w, 0); aw16(&w, 0); aw16(&w, 0);
    for (int i = 0; i < nt; i++) {
        aw8(&w, t[i].tag[0]); aw8(&w, t[i].tag[1]); aw8(&w, t[i].tag[2]); aw8(&w, t[i].tag[3]);
        aw32(&w, 0); aw32(&w, (uint32_t)t[i].off); aw32(&w, (uint32_t)t[i].len);
    }
    memcpy(out, w.d, total);
    return total;
#undef AB
#undef AE
}

/* A font set holding only Ahem, mapped to every generic family. data
 * must outlive the set. */
static AuFontSet *ahem_fontset(uint8_t *data)
{
    size_t n = ahem_build(data);
    AuFontSet *fs = au_fontset_new();
    au_fontset_add_data(fs, data, n);
    for (int g = 0; g < AU_GENERIC_COUNT; g++)
        au_fontset_set_generic(fs, (AuGenericFamily)g, "Ahem");
    return fs;
}

#endif
