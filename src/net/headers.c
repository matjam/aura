/* HTTP header lists and value parsers. */
#include "headers.h"

#include <stdio.h>
#include <string.h>

void au_headers_init(AuHeaders *h)
{
    h->items = NULL;
    h->n = h->cap = 0;
}

void au_headers_clear(AuHeaders *h)
{
    for (size_t i = 0; i < h->n; i++) {
        au_free(h->items[i].name);
        au_free(h->items[i].value);
    }
    h->n = 0;
}

void au_headers_free(AuHeaders *h)
{
    au_headers_clear(h);
    au_free(h->items);
    au_headers_init(h);
}

void au_headers_addn(AuHeaders *h, const char *name, size_t nlen, const char *value, size_t vlen)
{
    if (h->n == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 16;
        h->items = au_realloc(h->items, h->cap * sizeof *h->items);
    }
    h->items[h->n].name = au_strndup(name, nlen);
    h->items[h->n].value = au_strndup(value, vlen);
    h->n++;
}

void au_headers_add(AuHeaders *h, const char *name, const char *value)
{
    au_headers_addn(h, name, strlen(name), value, strlen(value));
}

void au_headers_copy(AuHeaders *dst, const AuHeaders *src)
{
    au_headers_init(dst);
    for (size_t i = 0; i < src->n; i++)
        au_headers_add(dst, src->items[i].name, src->items[i].value);
}

static bool name_eq(const char *a, const char *b)
{
    return au_ieq(a, strlen(a), b, strlen(b));
}

void au_headers_remove(AuHeaders *h, const char *name)
{
    size_t j = 0;
    for (size_t i = 0; i < h->n; i++) {
        if (name_eq(h->items[i].name, name)) {
            au_free(h->items[i].name);
            au_free(h->items[i].value);
        } else {
            h->items[j++] = h->items[i];
        }
    }
    h->n = j;
}

void au_headers_set(AuHeaders *h, const char *name, const char *value)
{
    au_headers_remove(h, name);
    au_headers_add(h, name, value);
}

const char *au_headers_get(const AuHeaders *h, const char *name)
{
    for (size_t i = 0; i < h->n; i++)
        if (name_eq(h->items[i].name, name))
            return h->items[i].value;
    return NULL;
}

const char *au_headers_next(const AuHeaders *h, const char *name, size_t *pos)
{
    for (size_t i = *pos; i < h->n; i++)
        if (name_eq(h->items[i].name, name)) {
            *pos = i + 1;
            return h->items[i].value;
        }
    *pos = h->n;
    return NULL;
}

bool au_headers_get_combined(const AuHeaders *h, const char *name, AuBuf *out)
{
    au_buf_clear(out);
    bool any = false;
    for (size_t i = 0; i < h->n; i++)
        if (name_eq(h->items[i].name, name)) {
            if (any)
                au_buf_puts(out, ", ");
            au_buf_puts(out, h->items[i].value);
            any = true;
        }
    au_buf_cstr(out);
    return any;
}

bool au_http_is_tchar(int c)
{
    if (au_is_alpha(c) || au_is_digit(c))
        return true;
    return c && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static bool ows(int c) { return c == ' ' || c == '\t'; }

bool au_http_list_next(const char **sp, const char **item, size_t *ilen)
{
    const char *s = *sp;
    for (;;) {
        while (*s && (ows((unsigned char)*s) || *s == ','))
            s++;
        if (!*s) {
            *sp = s;
            return false;
        }
        const char *start = s;
        bool q = false;
        while (*s && (q || *s != ',')) {
            if (*s == '"')
                q = !q;
            else if (*s == '\\' && q && s[1])
                s++;
            s++;
        }
        const char *end = s;
        while (end > start && ows((unsigned char)end[-1]))
            end--;
        *sp = s;
        if (end > start) {
            *item = start;
            *ilen = (size_t)(end - start);
            return true;
        }
    }
}

bool au_headers_has_token(const AuHeaders *h, const char *name, const char *token)
{
    size_t pos = 0, tl = strlen(token);
    const char *v;
    while ((v = au_headers_next(h, name, &pos)) != NULL) {
        const char *item;
        size_t il;
        while (au_http_list_next(&v, &item, &il))
            if (au_ieq(item, il, token, tl))
                return true;
    }
    return false;
}

/* Appends a parameter value at *s (token or quoted string) to out and
 * advances *s. */
static void param_value(const char **sp, AuBuf *out)
{
    const char *s = *sp;
    if (*s == '"') {
        s++;
        while (*s && *s != '"') {
            if (*s == '\\' && s[1])
                s++;
            au_buf_putc(out, *s++);
        }
        if (*s == '"')
            s++;
    } else {
        const char *start = s;
        while (*s && *s != ';')
            s++;
        const char *end = s;
        while (end > start && ows((unsigned char)end[-1]))
            end--;
        au_buf_append(out, start, (size_t)(end - start));
    }
    *sp = s;
}

bool au_http_param(const char *s, const char *name, AuBuf *out)
{
    size_t nl = strlen(name);
    /* Skip to the first ';'. */
    s = strchr(s, ';');
    while (s && *s == ';') {
        s++;
        while (ows((unsigned char)*s))
            s++;
        const char *pn = s;
        while (*s && *s != '=' && *s != ';')
            s++;
        const char *pe = s;
        while (pe > pn && ows((unsigned char)pe[-1]))
            pe--;
        if (*s == '=') {
            s++;
            while (ows((unsigned char)*s))
                s++;
            if (au_ieq(pn, (size_t)(pe - pn), name, nl)) {
                param_value(&s, out);
                au_buf_cstr(out);
                return true;
            }
            AuBuf skip;
            au_buf_init(&skip);
            param_value(&s, &skip);
            au_buf_free(&skip);
        }
        s = strchr(s, ';');
    }
    return false;
}

bool au_http_parse_mime(const char *s, AuBuf *essence, AuBuf *charset)
{
    au_buf_clear(essence);
    if (charset)
        au_buf_clear(charset);
    while (ows((unsigned char)*s) || *s == '\r' || *s == '\n')
        s++;
    const char *t = s;
    while (*s && au_http_is_tchar((unsigned char)*s))
        s++;
    if (s == t || *s != '/')
        return false;
    s++;
    const char *st = s;
    while (*s && au_http_is_tchar((unsigned char)*s))
        s++;
    if (s == st)
        return false;
    const char *e = s;
    while (ows((unsigned char)*s))
        s++;
    if (*s && *s != ';')
        return false;
    for (const char *p = t; p < e; p++)
        au_buf_putc(essence, (char)au_lower((unsigned char)*p));
    au_buf_cstr(essence);
    if (charset) {
        au_http_param(t, "charset", charset);
        au_buf_cstr(charset);
    }
    return true;
}

/* ---- Dates ---- */

int64_t au_days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static const char *const months[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                                       "jul", "aug", "sep", "oct", "nov", "dec"};

/* Parses the cookie-date algorithm's tokens too (RFC 6265 5.1.1), which
 * accepts all three HTTP date forms and the common variants: scan
 * delimiter-separated tokens for a time, a day of month, a month name and
 * a year. */
bool au_http_parse_date(const char *s, int64_t *t)
{
    int hour = -1, min = -1, sec = -1, day = -1, month = -1, year = -1;
    while (*s) {
        /* delimiters: everything but digits, letters and ':' */
        while (*s && !(au_is_alpha((unsigned char)*s) || au_is_digit((unsigned char)*s) || *s == ':'))
            s++;
        const char *tok = s;
        while (*s && (au_is_alpha((unsigned char)*s) || au_is_digit((unsigned char)*s) || *s == ':'))
            s++;
        size_t n = (size_t)(s - tok);
        if (!n)
            break;
        if (hour < 0) {
            int h, mi, se;
            char extra;
            char buf[16];
            if (n < sizeof buf) {
                memcpy(buf, tok, n);
                buf[n] = 0;
                if (sscanf(buf, "%2d:%2d:%2d%c", &h, &mi, &se, &extra) == 3 && au_is_digit((unsigned char)buf[0])) {
                    hour = h;
                    min = mi;
                    sec = se;
                    continue;
                }
            }
        }
        if (day < 0 && n <= 2 && au_is_digit((unsigned char)tok[0]) && (n == 1 || au_is_digit((unsigned char)tok[1]))) {
            day = tok[0] - '0';
            if (n == 2)
                day = day * 10 + tok[1] - '0';
            continue;
        }
        if (month < 0 && n >= 3) {
            for (int m = 0; m < 12; m++)
                if (au_ieq(tok, 3, months[m], 3))
                    month = m + 1;
            if (month > 0)
                continue;
        }
        if (year < 0 && (n == 2 || n == 4) && au_is_digit((unsigned char)tok[0]) && au_is_digit((unsigned char)tok[1])) {
            bool all = true;
            for (size_t i = 0; i < n; i++)
                all &= au_is_digit((unsigned char)tok[i]) != 0;
            if (all) {
                year = 0;
                for (size_t i = 0; i < n; i++)
                    year = year * 10 + tok[i] - '0';
                continue;
            }
        }
    }
    if (year >= 70 && year <= 99)
        year += 1900;
    else if (year >= 0 && year <= 69)
        year += 2000;
    if (day < 1 || day > 31 || month < 1 || year < 1601 || hour < 0 || hour > 23 || min < 0 || min > 59 ||
        sec < 0 || sec > 59)
        return false;
    *t = au_days_from_civil(year, (unsigned)month, (unsigned)day) * 86400 + hour * 3600 + min * 60 + sec;
    return true;
}

void au_http_format_date(int64_t t, char out[32])
{
    static const char *const wd[7] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
    static const char *const mn[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int64_t rem = t - days * 86400;
    /* civil_from_days */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    int w = (int)(((days % 7) + 7) % 7);
    snprintf(out, 32, "%s, %02u %s %04d %02d:%02d:%02d GMT", wd[w], d, mn[m - 1], (int)y, (int)(rem / 3600),
             (int)(rem / 60 % 60), (int)(rem % 60));
}
