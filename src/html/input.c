/* Input byte stream: BOM sniffing, decoding to UTF-8 (UTF-8, UTF-16 and
 * windows-1252 per the WHATWG Encoding Standard) and newline
 * normalization (the input stream preprocessing step).
 *
 * Encoding detection beyond the BOM goes in html_decode's sniffing step:
 * the <meta charset> prescan (HTML 13.2.3.2) would examine the first 1024
 * bytes there before choosing d->fallback. It is not implemented yet; the
 * default is UTF-8. */
#include "html_internal.h"

void html_decoder_init(HtmlDecoder *d, AuHtmlEncoding enc)
{
    d->enc = AU_HTML_ENC_SNIFF;
    d->fallback = (uint8_t)(enc == AU_HTML_ENC_SNIFF ? AU_HTML_ENC_UTF8 : enc);
    d->last_cr = false;
    d->nbom = 0;
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

void html_decode(HtmlDecoder *d, const char *bytes, size_t len, bool last, AuBuf *out)
{
    const unsigned char *s = (const unsigned char *)bytes;
    if (d->enc == AU_HTML_ENC_SNIFF) {
        /* BOM sniffing: hold up to three bytes until decided. */
        while (len && d->nbom < 3) {
            d->bom[d->nbom++] = *s++;
            len--;
        }
        size_t skip = 0;
        if (d->nbom >= 3 && d->bom[0] == 0xEF && d->bom[1] == 0xBB && d->bom[2] == 0xBF) {
            d->enc = AU_HTML_ENC_UTF8;
            skip = 3;
        } else if (d->nbom >= 2 && d->bom[0] == 0xFE && d->bom[1] == 0xFF) {
            d->enc = AU_HTML_ENC_UTF16BE;
            skip = 2;
        } else if (d->nbom >= 2 && d->bom[0] == 0xFF && d->bom[1] == 0xFE) {
            d->enc = AU_HTML_ENC_UTF16LE;
            skip = 2;
        } else if (d->nbom == 3 || last) {
            d->enc = d->fallback;
        } else {
            return; /* need more bytes to decide */
        }
        decode_bytes(d, d->bom + skip, d->nbom - skip, out);
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
