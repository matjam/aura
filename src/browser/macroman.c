/* Mac Roman conversions. See macroman.h. */
#include "macroman.h"

#include <string.h>

const uint16_t au_macroman_high[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5,
    0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4,
    0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6,
    0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265,
    0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF,
    0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5,
    0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044,
    0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9,
    0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

int au_macroman_byte(uint32_t cp)
{
    if (cp < 0x80)
        return (int)cp;
    for (int i = 0; i < 128; i++)
        if (au_macroman_high[i] == cp)
            return 0x80 + i;
    return -1;
}

/* Latin Extended-A (U+0100 to U+017F) without its diacritics; '*' marks
 * the ligatures IJ and ij, written as two letters. Letters Mac Roman has
 * (dotless i, OE, oe, Y with diaeresis) never get here. */
static const char latin_ext_a[129] = "AaAaAaCcCcCcCcDd"
                                     "DdEeEeEeEeEeGgGg"
                                     "GgGgHhHhIiIiIiIi"
                                     "Ii**JjKkkLlLlLlL"
                                     "lLlNnNnNnnNnOoOo"
                                     "OoOoRrRrRrSsSsSs"
                                     "SsTtTtTtUuUuUuUu"
                                     "UuUuWwYyYZzZzZzs";

/* Transliterations in code point order; the text is UTF-8 made of
 * characters Mac Roman has ("" drops the character). */
static const struct {
    uint16_t cp;
    const char *text;
} translit[] = {
    {0x00A6, "|"},     {0x00AD, ""},      {0x00B2, "2"},     {0x00B3, "3"},     {0x00B9, "1"},
    {0x00BC, " 1/4"},  {0x00BD, " 1/2"},  {0x00BE, " 3/4"},  {0x00D0, "D"},     {0x00D7, "x"},
    {0x00DD, "Y"},     {0x00DE, "Th"},    {0x00F0, "d"},     {0x00FD, "y"},     {0x00FE, "th"},
    {0x02B9, "'"},     {0x02BA, "\""},    {0x02BB, "\xE2\x80\x98"}, {0x02BC, "\xE2\x80\x99"},
    {0x02C8, "'"},     {0x02CB, "`"},     {0x02D0, ":"},
    {0x2010, "-"},     {0x2011, "-"},     {0x2012, "\xE2\x80\x93"}, {0x2015, "\xE2\x80\x94"},
    {0x2016, "||"},    {0x2017, "_"},     {0x201B, "\xE2\x80\x98"}, {0x201F, "\xE2\x80\x9C"},
    {0x2024, "."},     {0x2025, ".."},    {0x2027, "\xC2\xB7"},  {0x2028, "\n"},    {0x2029, "\n"},
    {0x202F, " "},     {0x2032, "'"},     {0x2033, "\""},    {0x2034, "'''"},   {0x2035, "`"},
    {0x2036, "``"},    {0x203C, "!!"},    {0x2043, "-"},     {0x2047, "??"},    {0x2048, "?!"},
    {0x2049, "!?"},    {0x2052, "%"},     {0x205F, " "},     {0x20A4, "\xC2\xA3"}, {0x20A9, "W"},
    {0x20B9, "Rs"},    {0x20BD, "R"},     {0x2103, "\xC2\xB0" "C"}, {0x2109, "\xC2\xB0" "F"},
    {0x2116, "No."},   {0x2117, "(P)"},   {0x2120, "SM"},    {0x2153, " 1/3"},  {0x2154, " 2/3"},
    {0x215B, " 1/8"},  {0x2190, "<-"},    {0x2191, "^"},     {0x2192, "->"},    {0x2193, "v"},
    {0x2194, "<->"},   {0x21D0, "<="},    {0x21D2, "=>"},    {0x21D4, "<=>"},   {0x2212, "-"},
    {0x2213, "-/+"},   {0x2215, "/"},     {0x2216, "\\"},    {0x2217, "*"},     {0x2218, "\xC2\xB0"},
    {0x2219, "\xE2\x80\xA2"}, {0x2223, "|"}, {0x2225, "||"}, {0x2236, ":"},    {0x223C, "~"},
    {0x2245, "\xE2\x89\x88"}, {0x2261, "="}, {0x226A, "<<"}, {0x226B, ">>"},   {0x22C5, "\xC2\xB7"},
    {0x22EF, "\xE2\x80\xA6"}, {0x2329, "<"}, {0x232A, ">"},  {0x2500, "-"},    {0x2502, "|"},
    {0x25AA, "\xE2\x80\xA2"}, {0x25AB, "\xE2\x80\xA2"}, {0x25B2, "^"}, {0x25B6, ">"},
    {0x25BA, ">"},     {0x25BC, "v"},     {0x25C0, "<"},     {0x25C4, "<"},     {0x25CB, "o"},
    {0x25CF, "\xE2\x80\xA2"}, {0x25E6, "\xE2\x80\xA2"}, {0x2605, "*"}, {0x2606, "*"},
    {0x2610, "[ ]"},   {0x2611, "[x]"},   {0x2612, "[x]"},   {0x2713, "\xE2\x88\x9A"},
    {0x2714, "\xE2\x88\x9A"}, {0x2715, "x"}, {0x2716, "x"},  {0x2717, "x"},    {0x2718, "x"},
    {0x27E8, "<"},     {0x27E9, ">"},     {0x3000, " "},     {0x3001, ","},     {0x3002, "."},
    {0xFE50, ","},     {0xFE52, "."},     {0xFEFF, ""},
};

static const char *find_translit(uint32_t cp)
{
    /* Linear: the table is short, and only characters Mac Roman lacks get
     * here. */
    for (size_t i = 0; i < sizeof translit / sizeof translit[0]; i++)
        if (translit[i].cp == cp)
            return translit[i].text;
    return NULL;
}

static void put_byte(AuBuf *out, int b, unsigned flags)
{
    if ((flags & AU_MR_CR_LINES) && b == '\n')
        b = '\r';
    au_buf_putc(out, (char)b);
}

/* Appends the Mac Roman form of one code point. */
static void put_cp(AuBuf *out, uint32_t cp, char missing, unsigned flags)
{
    int b = au_macroman_byte(cp);
    if (b >= 0 && cp != 0xFFFD) {
        put_byte(out, b, flags);
        return;
    }
    /* Invisible characters: zero-width spaces and joiners, direction
     * marks, word joiners, combining marks, variation selectors. */
    if ((cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064) ||
        (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F) || (cp >= 0xE0100 && cp <= 0xE01EF))
        return;
    /* Spaces of every width. */
    if ((cp >= 0x2000 && cp <= 0x200A) || cp == 0x1680) {
        put_byte(out, ' ', flags);
        return;
    }
    /* Fullwidth ASCII. */
    if (cp >= 0xFF01 && cp <= 0xFF5E) {
        put_byte(out, (int)(cp - 0xFEE0), flags);
        return;
    }
    if (cp >= 0x0100 && cp <= 0x017F) {
        char c = latin_ext_a[cp - 0x100];
        if (c == '*') {
            put_byte(out, cp == 0x132 ? 'I' : 'i', flags);
            put_byte(out, cp == 0x132 ? 'J' : 'j', flags);
        } else {
            put_byte(out, (unsigned char)c, flags);
        }
        return;
    }
    const char *t = find_translit(cp);
    if (t) {
        size_t n = strlen(t);
        for (size_t i = 0; i < n;) {
            size_t adv;
            uint32_t c = au_utf8_decode(t + i, n - i, &adv);
            i += adv;
            int tb = au_macroman_byte(c);
            put_byte(out, tb >= 0 ? tb : (unsigned char)missing, flags);
        }
        return;
    }
    put_byte(out, (unsigned char)missing, flags);
}

void au_macroman_from_utf8(const char *s, size_t n, AuBuf *out, char missing, unsigned flags)
{
    for (size_t i = 0; i < n;) {
        size_t adv;
        uint32_t cp = au_utf8_decode(s + i, n - i, &adv);
        if ((flags & AU_MR_CR_LINES) && cp == '\r' && i + 1 < n && s[i + 1] == '\n') {
            i++; /* CR LF: one line break */
            continue;
        }
        i += adv;
        put_cp(out, cp, missing, flags);
    }
}

void au_macroman_to_utf8(const char *s, size_t n, AuBuf *out, unsigned flags)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\r' && (flags & AU_MR_CR_LINES))
            au_buf_putc(out, '\n');
        else
            au_buf_put_utf8(out, c < 0x80 ? c : au_macroman_high[c - 0x80]);
    }
}

static void put16(AuBuf *out, uint32_t u)
{
    char b[2] = {(char)(u >> 8), (char)(u & 0xFF)};
    au_buf_append(out, b, 2);
}

void au_utf16be_from_utf8(const char *s, size_t n, AuBuf *out, unsigned flags)
{
    for (size_t i = 0; i < n;) {
        size_t adv;
        uint32_t cp = au_utf8_decode(s + i, n - i, &adv);
        if ((flags & AU_MR_CR_LINES) && cp == '\r' && i + 1 < n && s[i + 1] == '\n') {
            i++;
            continue;
        }
        i += adv;
        if ((flags & AU_MR_CR_LINES) && cp == '\n')
            cp = '\r';
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
            cp -= 0x10000;
            put16(out, 0xD800 | (cp >> 10));
            put16(out, 0xDC00 | (cp & 0x3FF));
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            put16(out, 0xFFFD);
        } else {
            put16(out, cp);
        }
    }
}
