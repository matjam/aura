/* The Fonts folder index (fonts.idx): one line per face, so platforms
 * register bundled faces without opening the font files. */
#include "font.h"

#include <stdlib.h>
#include <string.h>

/* The next tab-separated field of [*p, end), advancing *p past its tab;
 * *p becomes NULL after the last field. */
static bool field(const char **p, const char *end, const char **s, size_t *n)
{
    if (!*p)
        return false;
    const char *q = *p;
    while (q < end && *q != '\t')
        q++;
    *s = *p;
    *n = (size_t)(q - *p);
    *p = q < end ? q + 1 : NULL;
    return true;
}

static bool parse_int(const char *s, size_t n, int lo, int hi, int *out)
{
    if (!n || n > 6)
        return false;
    int v = 0;
    for (size_t i = 0; i < n; i++) {
        if (!au_is_digit((unsigned char)s[i]))
            return false;
        v = v * 10 + (s[i] - '0');
    }
    if (v < lo || v > hi)
        return false;
    *out = v;
    return true;
}

/* A relative path of plain components: no empty or dot components, no
 * leading /, no characters the platforms' paths reserve. */
static bool safe_path(const char *s, size_t n)
{
    if (!n || n >= sizeof(((AuFontIndexEntry *)0)->path) || s[0] == '/')
        return false;
    size_t comp = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || s[i] == '/') {
            if (comp == 0 || comp > 31)
                return false;
            const char *c = s + i - comp;
            if (c[0] == '.')
                return false;
            comp = 0;
            continue;
        }
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch == ':' || ch == '\\' || ch == 0x7F)
            return false;
        comp++;
    }
    return true;
}

int au_font_index_parse(const char *text, size_t len, AuFontIndexFn fn, void *ctx)
{
    int count = 0;
    const char *p = text, *end = text + len;
    while (p < end) {
        const char *eol = p;
        while (eol < end && *eol != '\n' && *eol != '\r')
            eol++;
        const char *line = p;
        p = eol < end ? eol + 1 : end;
        if (eol == line || line[0] == '#')
            continue;
        const char *q = line, *s[5];
        size_t n[5];
        bool ok = true;
        for (int k = 0; k < 5 && ok; k++)
            ok = field(&q, eol, &s[k], &n[k]);
        if (!ok || q)
            continue; /* too few or too many fields */
        AuFontIndexEntry e;
        memset(&e, 0, sizeof e);
        if (!safe_path(s[0], n[0]) || !parse_int(s[1], n[1], 0, 1023, &e.face_index) ||
            !parse_int(s[2], n[2], 1, 1000, &e.weight) || !n[4] || n[4] >= sizeof e.family)
            continue;
        if (n[3] == 6 && !memcmp(s[3], "normal", 6))
            e.italic = false;
        else if (n[3] == 6 && !memcmp(s[3], "italic", 6))
            e.italic = true;
        else
            continue;
        bool printable = true;
        for (size_t i = 0; i < n[4]; i++)
            if ((unsigned char)s[4][i] < 0x20)
                printable = false;
        if (!printable)
            continue;
        memcpy(e.path, s[0], n[0]);
        memcpy(e.family, s[4], n[4]);
        fn(ctx, &e);
        count++;
    }
    return count;
}

int au_font_index_write(AuBuf *b, const char *path, const void *data, size_t len)
{
    int n = au_font_face_count(data, len), added = 0;
    for (int i = 0; i < n; i++) {
        AuFontFace *f = au_font_face_new(data, len, i);
        if (!f)
            continue;
        au_buf_printf(b, "%s\t%d\t%d\t%s\t%s\n", path, i, (int)f->weight, f->italic ? "italic" : "normal", f->family);
        au_font_face_free(f);
        added++;
    }
    return added;
}
