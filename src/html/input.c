/* Input byte stream: BOM sniffing, decoding to UTF-8 (UTF-8, UTF-16 and
 * windows-1252 per the WHATWG Encoding Standard) and newline
 * normalization (the input stream preprocessing step).
 *
 * Encoding detection beyond the BOM: with AU_HTML_ENC_SNIFF, the <meta
 * charset> prescan (HTML 13.2.3.2, au_html_prescan_charset below) examines
 * the first 1024 bytes, which the decoder holds back (in any number of
 * chunks) until it has them or the input ends; a label it finds that this
 * decoder supports (UTF-8, UTF-16 meaning UTF-8, the windows-1252 family)
 * decides the encoding. Otherwise the default is UTF-8. A network loader
 * that passes an explicit encoding gets the full algorithm (src/net:
 * au_detect_encoding). */
#include "html_internal.h"

#include <string.h>

/* d->fallback holds the encoding for "no BOM" in its low bits; this bit
 * marks a fallback that came from SNIFF, so the prescan may override it. */
#define FALLBACK_SNIFFED 0x80

void html_decoder_init(HtmlDecoder *d, AuHtmlEncoding enc)
{
    d->enc = AU_HTML_ENC_SNIFF;
    d->fallback = (uint8_t)(enc == AU_HTML_ENC_SNIFF ? (AU_HTML_ENC_UTF8 | FALLBACK_SNIFFED) : enc);
    d->last_cr = false;
    d->nhold = 0;
    d->need = d->seen = 0;
    d->lower = 0x80;
    d->upper = 0xBF;
    d->cp = 0;
    d->lead_byte = -1;
    d->lead_surrogate = 0;
}

static void put_cp(HtmlDecoder *d, uint32_t cp, AuBuf *out)
{
    if (cp == '\r') {
        au_buf_putc(out, '\n');
        d->last_cr = true;
        return;
    }
    if (cp == '\n' && d->last_cr) {
        d->last_cr = false;
        return;
    }
    d->last_cr = false;
    if (cp < 0x80)
        au_buf_putc(out, (char)cp);
    else
        au_buf_put_utf8(out, cp);
}

static const uint16_t win1252_high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

static void decode_utf8(HtmlDecoder *d, const unsigned char *s, size_t n, AuBuf *out)
{
    size_t i = 0;
    while (i < n) {
        unsigned b = s[i];
        if (d->need == 0) {
            if (b < 0x80) {
                /* Fast path: a run of ASCII without CR. */
                size_t j = i;
                while (j < n && s[j] < 0x80 && s[j] != '\r' && s[j] != '\n')
                    j++;
                if (j > i) {
                    d->last_cr = false;
                    au_buf_append(out, s + i, j - i);
                    i = j;
                    continue;
                }
                put_cp(d, b, out);
            } else if (b >= 0xC2 && b <= 0xDF) {
                d->need = 1;
                d->cp = b & 0x1F;
            } else if (b >= 0xE0 && b <= 0xEF) {
                if (b == 0xE0)
                    d->lower = 0xA0;
                if (b == 0xED)
                    d->upper = 0x9F;
                d->need = 2;
                d->cp = b & 0x0F;
            } else if (b >= 0xF0 && b <= 0xF4) {
                if (b == 0xF0)
                    d->lower = 0x90;
                if (b == 0xF4)
                    d->upper = 0x8F;
                d->need = 3;
                d->cp = b & 0x07;
            } else {
                put_cp(d, 0xFFFD, out);
            }
            i++;
            continue;
        }
        if (b < d->lower || b > d->upper) {
            /* Malformed: emit U+FFFD and reprocess this byte. */
            d->cp = d->need = d->seen = 0;
            d->lower = 0x80;
            d->upper = 0xBF;
            put_cp(d, 0xFFFD, out);
            continue;
        }
        d->lower = 0x80;
        d->upper = 0xBF;
        d->cp = (d->cp << 6) | (b & 0x3F);
        d->seen++;
        i++;
        if (d->seen == d->need) {
            put_cp(d, d->cp, out);
            d->cp = d->need = d->seen = 0;
        }
    }
}

static void decode_utf16(HtmlDecoder *d, const unsigned char *s, size_t n, bool be, AuBuf *out)
{
    for (size_t i = 0; i < n; i++) {
        if (d->lead_byte < 0) {
            d->lead_byte = s[i];
            continue;
        }
        uint32_t unit = be ? ((uint32_t)d->lead_byte << 8) | s[i] : ((uint32_t)s[i] << 8) | (uint32_t)d->lead_byte;
        d->lead_byte = -1;
        if (d->lead_surrogate) {
            uint32_t lead = d->lead_surrogate;
            d->lead_surrogate = 0;
            if (unit >= 0xDC00 && unit <= 0xDFFF) {
                put_cp(d, 0x10000 + ((lead - 0xD800) << 10) + (unit - 0xDC00), out);
                continue;
            }
            put_cp(d, 0xFFFD, out);
        }
        if (unit >= 0xD800 && unit <= 0xDBFF)
            d->lead_surrogate = unit;
        else if (unit >= 0xDC00 && unit <= 0xDFFF)
            put_cp(d, 0xFFFD, out);
        else
            put_cp(d, unit, out);
    }
}

static void decode_bytes(HtmlDecoder *d, const unsigned char *s, size_t n, AuBuf *out)
{
    switch (d->enc) {
    case AU_HTML_ENC_UTF16LE:
        decode_utf16(d, s, n, false, out);
        break;
    case AU_HTML_ENC_UTF16BE:
        decode_utf16(d, s, n, true, out);
        break;
    case AU_HTML_ENC_WINDOWS_1252:
        for (size_t i = 0; i < n; i++) {
            unsigned b = s[i];
            put_cp(d, b >= 0x80 && b < 0xA0 ? win1252_high[b - 0x80] : b, out);
        }
        break;
    default:
        decode_utf8(d, s, n, out);
        break;
    }
}

/* ---- The <meta charset> prescan (HTML 13.2.3.2) ---- */

typedef struct Scan {
    const uint8_t *d;
    size_t n, pos;
} Scan;

static bool scan_ws(int c) { return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20; }

static bool scan_starts_ci(const Scan *s, const char *lit)
{
    size_t l = strlen(lit);
    if (s->n - s->pos < l)
        return false;
    for (size_t i = 0; i < l; i++)
        if (au_lower(s->d[s->pos + i]) != lit[i])
            return false;
    return true;
}

#define ATTR_MAX 512

/* "Get an attribute". Returns false when there is none (or the input
 * ended). Names and values are lowercased ASCII, truncated at ATTR_MAX. */
static bool get_attribute(Scan *s, char *name, size_t *nl, char *value, size_t *vl)
{
    *nl = *vl = 0;
    while (s->pos < s->n && (scan_ws(s->d[s->pos]) || s->d[s->pos] == '/'))
        s->pos++;
    if (s->pos >= s->n || s->d[s->pos] == '>')
        return false;
    /* name */
    for (;;) {
        if (s->pos >= s->n)
            return false;
        int c = s->d[s->pos];
        if (c == '=' && *nl > 0) {
            s->pos++;
            goto value;
        }
        if (scan_ws(c))
            break;
        if (c == '/' || c == '>')
            return true;
        if (*nl < ATTR_MAX)
            name[(*nl)++] = (char)au_lower(c);
        s->pos++;
    }
    /* spaces */
    while (s->pos < s->n && scan_ws(s->d[s->pos]))
        s->pos++;
    if (s->pos >= s->n)
        return false;
    if (s->d[s->pos] != '=')
        return true;
    s->pos++;
value:
    while (s->pos < s->n && scan_ws(s->d[s->pos]))
        s->pos++;
    if (s->pos >= s->n)
        return false;
    {
        int c = s->d[s->pos];
        if (c == '"' || c == '\'') {
            int q = c;
            for (;;) {
                s->pos++;
                if (s->pos >= s->n)
                    return false;
                c = s->d[s->pos];
                if (c == q) {
                    s->pos++;
                    return true;
                }
                if (*vl < ATTR_MAX)
                    value[(*vl)++] = (char)au_lower(c);
            }
        }
        if (c == '>')
            return true;
        if (*vl < ATTR_MAX)
            value[(*vl)++] = (char)au_lower(c);
        s->pos++;
    }
    for (;;) {
        if (s->pos >= s->n)
            return false;
        int c = s->d[s->pos];
        if (scan_ws(c) || c == '>')
            return true;
        if (*vl < ATTR_MAX)
            value[(*vl)++] = (char)au_lower(c);
        s->pos++;
    }
}

/* "Extract a character encoding from a meta element" (from content). */
static bool extract_charset(const char *s, size_t n, size_t *start, size_t *len)
{
    size_t p = 0;
    for (;;) {
        /* find "charset" */
        size_t i = p;
        bool found = false;
        for (; i + 7 <= n; i++)
            if (au_ieq(s + i, 7, "charset", 7)) {
                found = true;
                break;
            }
        if (!found)
            return false;
        p = i + 7;
        while (p < n && scan_ws((unsigned char)s[p]))
            p++;
        if (p >= n || s[p] != '=')
            continue;
        p++;
        while (p < n && scan_ws((unsigned char)s[p]))
            p++;
        if (p >= n)
            return false;
        if (s[p] == '"' || s[p] == '\'') {
            char q = s[p];
            const char *e = memchr(s + p + 1, q, n - p - 1);
            if (!e)
                return false;
            *start = p + 1;
            *len = (size_t)(e - (s + p + 1));
            return true;
        }
        size_t b = p;
        while (p < n && !scan_ws((unsigned char)s[p]) && s[p] != ';')
            p++;
        *start = b;
        *len = p - b;
        return true;
    }
}

/* The labels this decoder can use, mapped to its encodings (a subset of
 * the Encoding Standard's table: UTF-8, UTF-16 and windows-1252). */
static int label_encoding(const char *s, size_t n)
{
    while (n && scan_ws((unsigned char)*s)) {
        s++;
        n--;
    }
    while (n && scan_ws((unsigned char)s[n - 1]))
        n--;
    static const char *const utf8[] = {"unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8", "utf-8", "utf8",
                                       "x-unicode20utf8"};
    static const char *const w1252[] = {"ansi_x3.4-1968", "ascii", "cp1252", "cp819", "csisolatin1", "ibm819",
                                        "iso-8859-1", "iso-ir-100", "iso8859-1", "iso88591", "iso_8859-1",
                                        "iso_8859-1:1987", "l1", "latin1", "us-ascii", "windows-1252",
                                        "x-cp1252", "x-user-defined"};
    static const char *const utf16[] = {"csunicode", "iso-10646-ucs-2", "ucs-2", "unicode", "unicodefeff",
                                        "utf-16", "utf-16le", "unicodefffe", "utf-16be"};
    for (size_t i = 0; i < sizeof utf8 / sizeof utf8[0]; i++)
        if (au_ieq(s, n, utf8[i], strlen(utf8[i])))
            return AU_HTML_ENC_UTF8;
    for (size_t i = 0; i < sizeof utf16 / sizeof utf16[0]; i++)
        if (au_ieq(s, n, utf16[i], strlen(utf16[i])))
            return AU_HTML_ENC_UTF8; /* a meta saying UTF-16 means UTF-8 */
    for (size_t i = 0; i < sizeof w1252 / sizeof w1252[0]; i++)
        if (au_ieq(s, n, w1252[i], strlen(w1252[i])))
            return AU_HTML_ENC_WINDOWS_1252;
    return -1;
}

static bool default_valid(void *ctx, const char *label, size_t n)
{
    (void)ctx;
    return label_encoding(label, n) >= 0;
}

/* Runs the prescan over the first 1024 bytes of data. valid() says
 * whether a label names a supported encoding (NULL: the labels above);
 * the first meta whose label is valid wins. Writes that label (as found,
 * lowercased, at most cap - 1 bytes, NUL-terminated) to out and returns
 * its length, or returns 0 when there is none. */
size_t au_html_prescan_charset(const uint8_t *data, size_t len, bool (*valid)(void *ctx, const char *label, size_t n),
                               void *ctx, char *out, size_t cap)
{
    Scan s = {data, len > 1024 ? 1024 : len, 0};
    if (!valid)
        valid = default_valid;
    char name[ATTR_MAX], value[ATTR_MAX];
    size_t nl, vl;
    while (s.pos < s.n) {
        if (scan_starts_ci(&s, "<!--")) {
            /* to the '>' of the first "-->" after the '<' */
            size_t p = s.pos + 2;
            while (p + 2 < s.n && !(s.d[p] == '-' && s.d[p + 1] == '-' && s.d[p + 2] == '>'))
                p++;
            if (p + 2 >= s.n)
                return 0;
            s.pos = p + 3;
            continue;
        }
        if (scan_starts_ci(&s, "<meta") && s.pos + 5 < s.n && (scan_ws(s.d[s.pos + 5]) || s.d[s.pos + 5] == '/')) {
            s.pos += 6;
            bool seen_he = false, seen_content = false, seen_charset = false, got_pragma = false;
            int need_pragma = -1; /* -1 null, 0 false, 1 true */
            char charset[ATTR_MAX];
            size_t cl = 0;
            bool have_charset = false;
            while (get_attribute(&s, name, &nl, value, &vl)) {
                if (nl == 10 && !memcmp(name, "http-equiv", 10)) {
                    if (seen_he)
                        continue;
                    seen_he = true;
                    if (vl == 12 && !memcmp(value, "content-type", 12))
                        got_pragma = true;
                } else if (nl == 7 && !memcmp(name, "content", 7)) {
                    if (seen_content)
                        continue;
                    seen_content = true;
                    size_t st, ln;
                    if (!have_charset && extract_charset(value, vl, &st, &ln)) {
                        memcpy(charset, value + st, ln);
                        cl = ln;
                        have_charset = true;
                        need_pragma = 1;
                    }
                } else if (nl == 7 && !memcmp(name, "charset", 7)) {
                    if (seen_charset)
                        continue;
                    seen_charset = true;
                    memcpy(charset, value, vl);
                    cl = vl;
                    have_charset = true;
                    need_pragma = 0;
                }
            }
            if (need_pragma == -1 || (need_pragma == 1 && !got_pragma) || !have_charset ||
                !valid(ctx, charset, cl)) {
                s.pos++;
                continue;
            }
            size_t n = cl < cap - 1 ? cl : cap - 1;
            memcpy(out, charset, n);
            out[n] = 0;
            return n ? n : 0;
        }
        if (s.d[s.pos] == '<' && s.pos + 1 < s.n &&
            (au_is_alpha(s.d[s.pos + 1]) || (s.d[s.pos + 1] == '/' && s.pos + 2 < s.n && au_is_alpha(s.d[s.pos + 2])))) {
            while (s.pos < s.n && !scan_ws(s.d[s.pos]) && s.d[s.pos] != '>')
                s.pos++;
            while (get_attribute(&s, name, &nl, value, &vl))
                ;
            s.pos++;
            continue;
        }
        if (scan_starts_ci(&s, "<!") || scan_starts_ci(&s, "</") || scan_starts_ci(&s, "<?")) {
            const uint8_t *gt = memchr(s.d + s.pos + 2, '>', s.n - s.pos - 2);
            if (!gt)
                return 0;
            s.pos = (size_t)(gt - s.d) + 1;
            continue;
        }
        s.pos++;
    }
    return 0;
}

/* Runs the prescan over the held bytes (the first 1024, or the whole
 * input when it is shorter). */
static void prescan_fallback(HtmlDecoder *d)
{
    d->fallback &= (uint8_t)~FALLBACK_SNIFFED;
    char label[64];
    size_t ll = au_html_prescan_charset(d->hold, d->nhold, NULL, NULL, label, sizeof label);
    if (ll) {
        int e = label_encoding(label, ll);
        if (e >= 0)
            d->fallback = (uint8_t)e;
    }
}

void html_decode(HtmlDecoder *d, const char *bytes, size_t len, bool last, AuBuf *out)
{
    const unsigned char *s = (const unsigned char *)bytes;
    if (d->enc == AU_HTML_ENC_SNIFF) {
        /* Hold the first bytes: up to three to find a BOM, and, when the
         * encoding is sniffed, up to 1024 for the prescan, until the input
         * ends. Nothing is decoded before the encoding is decided. */
        size_t room = sizeof d->hold - d->nhold;
        size_t take = len < room ? len : room;
        memcpy(d->hold + d->nhold, s, take);
        d->nhold = (uint16_t)(d->nhold + take);
        s += take;
        len -= take;
        size_t skip = 0;
        const unsigned char *h = d->hold;
        if (d->nhold >= 3 && h[0] == 0xEF && h[1] == 0xBB && h[2] == 0xBF) {
            d->enc = AU_HTML_ENC_UTF8;
            skip = 3;
        } else if (d->nhold >= 2 && h[0] == 0xFE && h[1] == 0xFF) {
            d->enc = AU_HTML_ENC_UTF16BE;
            skip = 2;
        } else if (d->nhold >= 2 && h[0] == 0xFF && h[1] == 0xFE) {
            d->enc = AU_HTML_ENC_UTF16LE;
            skip = 2;
        } else if (d->nhold < 3 && !last) {
            return; /* need more bytes to look for a BOM */
        } else if (!(d->fallback & FALLBACK_SNIFFED)) {
            d->enc = d->fallback;
        } else if (d->nhold < sizeof d->hold && !last) {
            return; /* hold the bytes the prescan looks at */
        } else {
            prescan_fallback(d);
            d->enc = d->fallback;
        }
        size_t n = d->nhold;
        d->nhold = 0;
        decode_bytes(d, d->hold + skip, n - skip, out);
    }
    decode_bytes(d, s, len, out);
    if (last) {
        if (d->need || d->lead_byte >= 0 || d->lead_surrogate)
            put_cp(d, 0xFFFD, out);
        d->need = d->seen = 0;
        d->lead_byte = -1;
        d->lead_surrogate = 0;
    }
}

void html_normalize_newlines(const char *s, size_t len, AuBuf *out)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r') {
            au_buf_putc(out, '\n');
            if (i + 1 < len && s[i + 1] == '\n')
                i++;
        } else {
            au_buf_putc(out, s[i]);
        }
    }
}
