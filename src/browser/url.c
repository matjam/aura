/* Address-field URL helpers. See url.h for what this does and does not
 * do. */
#include "url.h"

#include <string.h>

static bool scheme_char(int c)
{
    return au_is_alpha(c) || au_is_digit(c) || c == '+' || c == '-' || c == '.';
}

size_t au_url_scheme_len(const char *s, size_t n)
{
    if (n == 0 || !au_is_alpha((unsigned char)s[0]))
        return 0;
    for (size_t i = 1; i < n; i++) {
        if (s[i] == ':')
            return i;
        if (!scheme_char((unsigned char)s[i]))
            return 0;
    }
    return 0;
}

bool au_url_scheme_is(const char *url, const char *scheme)
{
    size_t n = strlen(url);
    size_t k = au_url_scheme_len(url, n);
    return k && au_ieq(url, k, scheme, strlen(scheme));
}

static const char *const known_schemes[] = {"http",  "https",  "file",   "about",      "ftp",
                                            "gopher", "data", "mailto", "javascript", "view-source"};

static bool known_scheme(const char *s, size_t n)
{
    for (size_t i = 0; i < sizeof known_schemes / sizeof known_schemes[0]; i++)
        if (au_ieq(s, n, known_schemes[i], strlen(known_schemes[i])))
            return true;
    return false;
}

static bool all_digits(const char *s, size_t n)
{
    if (n == 0)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!au_is_digit((unsigned char)s[i]))
            return false;
    return true;
}

static bool ipv4(const char *s, size_t n)
{
    int parts = 0;
    size_t i = 0;
    while (i <= n) {
        size_t j = i;
        while (j < n && s[j] != '.')
            j++;
        if (!all_digits(s + i, j - i) || j - i > 3)
            return false;
        int v = 0;
        for (size_t k = i; k < j; k++)
            v = v * 10 + (s[k] - '0');
        if (v > 255)
            return false;
        parts++;
        if (j == n)
            break;
        i = j + 1;
    }
    return parts == 4;
}

bool au_url_looks_like_host(const char *s, size_t n)
{
    if (n == 0)
        return false;
    /* Split off ":port". IPv6 addresses are bracketed, so the port colon
     * is the one after ']'. */
    size_t host_end = n;
    if (s[0] == '[') {
        const char *close = memchr(s, ']', n);
        if (!close)
            return false;
        host_end = (size_t)(close - s) + 1;
        if (host_end < n && (s[host_end] != ':' || !all_digits(s + host_end + 1, n - host_end - 1)))
            return false;
        for (size_t i = 1; i + 1 < host_end; i++)
            if (!au_is_hex((unsigned char)s[i]) && s[i] != ':' && s[i] != '.')
                return false;
        return host_end > 2;
    }
    const char *colon = memchr(s, ':', n);
    if (colon) {
        host_end = (size_t)(colon - s);
        if (!all_digits(colon + 1, n - host_end - 1) || n - host_end - 1 > 5)
            return false;
    }
    if (host_end == 0)
        return false;
    if (au_ieq(s, host_end, "localhost", 9))
        return true;
    if (ipv4(s, host_end))
        return true;
    /* Labels of letters, digits and '-', at least two of them, the last
     * at least two letters (a TLD). A trailing dot is allowed. */
    size_t end = host_end;
    if (s[end - 1] == '.')
        end--;
    int labels = 0;
    size_t i = 0, last_start = 0;
    while (i < end) {
        size_t j = i;
        while (j < end && s[j] != '.') {
            int c = (unsigned char)s[j];
            if (!au_is_alpha(c) && !au_is_digit(c) && c != '-' && c < 0x80)
                return false;
            j++;
        }
        if (j == i || s[i] == '-' || s[j - 1] == '-')
            return false;
        labels++;
        last_start = i;
        if (j == end)
            break;
        i = j + 1;
    }
    if (labels < 2)
        return false;
    size_t tld = end - last_start;
    if (tld < 2)
        return false;
    for (size_t k = last_start; k < end; k++)
        if (!au_is_alpha((unsigned char)s[k]) && (unsigned char)s[k] < 0x80)
            return false;
    return true;
}

void au_url_form_encode(AuBuf *out, const char *s, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (au_is_alpha(c) || au_is_digit(c) || c == '*' || c == '-' || c == '.' || c == '_') {
            au_buf_putc(out, (char)c);
        } else if (c == ' ') {
            au_buf_putc(out, '+');
        } else {
            au_buf_putc(out, '%');
            au_buf_putc(out, hex[c >> 4]);
            au_buf_putc(out, hex[c & 15]);
        }
    }
}

static int hexval(int c)
{
    if (au_is_digit(c))
        return c - '0';
    c = au_lower(c);
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

void au_url_percent_decode(AuBuf *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n) {
            int h = hexval((unsigned char)s[i + 1]), l = hexval((unsigned char)s[i + 2]);
            if (h >= 0 && l >= 0) {
                au_buf_putc(out, (char)(h * 16 + l));
                i += 2;
                continue;
            }
        }
        au_buf_putc(out, s[i]);
    }
}

/* Characters left as they are in a file URL path segment. */
static bool path_char(int c)
{
    return au_is_alpha(c) || au_is_digit(c) || strchr("-._~!$&'()*+,;=@", c) != NULL;
}

char *au_url_from_path(const char *path, char sep)
{
    static const char hex[] = "0123456789ABCDEF";
    AuBuf b;
    au_buf_init(&b);
    au_buf_puts(&b, "file://");
    const char *p = path;
    if (sep == '/') {
        if (*p != '/')
            au_buf_putc(&b, '/');
    } else {
        au_buf_putc(&b, '/');
    }
    for (; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == (unsigned char)sep) {
            au_buf_putc(&b, '/');
        } else if (path_char(c) && c != 0) {
            au_buf_putc(&b, (char)c);
        } else {
            au_buf_putc(&b, '%');
            au_buf_putc(&b, hex[c >> 4]);
            au_buf_putc(&b, hex[c & 15]);
        }
    }
    return au_buf_take(&b, NULL);
}

char *au_url_to_path(const char *url, char sep)
{
    if (!au_url_scheme_is(url, "file"))
        return NULL;
    const char *p = url + 5;
    if (strncmp(p, "//", 2) == 0) {
        p += 2;
        if (au_ieq(p, strlen(p) < 9 ? strlen(p) : 9, "localhost", 9))
            p += 9;
        else if (*p != '/')
            return NULL; /* a remote host */
    }
    if (*p != '/')
        return NULL;
    size_t n = strcspn(p, "?#");
    AuBuf dec;
    au_buf_init(&dec);
    au_url_percent_decode(&dec, p, n);
    au_buf_cstr(&dec);
    AuBuf out;
    au_buf_init(&out);
    const char *d = dec.data;
    if (sep == '/') {
        au_buf_append(&out, d, dec.len);
    } else {
        /* "/Volume/a/b" -> "Volume:a:b"; a '/' that was percent-encoded in
         * a name stays a '/', but HFS names rarely hold one and Aura does
         * not track which slashes were encoded. */
        for (size_t i = 1; i < dec.len; i++)
            au_buf_putc(&out, d[i] == '/' ? sep : d[i]);
    }
    au_buf_free(&dec);
    if (out.len == 0) {
        au_buf_free(&out);
        return NULL;
    }
    return au_buf_take(&out, NULL);
}

const char *au_url_match_key(const char *url, size_t *len)
{
    size_t n = strlen(url);
    size_t k = au_url_scheme_len(url, n);
    const char *p = url;
    if (k) {
        p += k + 1;
        if (strncmp(p, "//", 2) == 0)
            p += 2;
    }
    if (strncmp(p, "www.", 4) == 0)
        p += 4;
    *len = strlen(p);
    return p;
}

void au_url_host(const char *url, char *out, size_t cap)
{
    out[0] = 0;
    size_t n = strlen(url);
    size_t k = au_url_scheme_len(url, n);
    if (!k || strncmp(url + k + 1, "//", 2) != 0)
        return;
    const char *h = url + k + 3;
    size_t m = strcspn(h, "/?#");
    const char *at = memchr(h, '@', m);
    if (at) {
        m -= (size_t)(at + 1 - h);
        h = at + 1;
    }
    if (m >= cap)
        m = cap - 1;
    memcpy(out, h, m);
    out[m] = 0;
}

static bool is_local_path(const char *s, size_t n)
{
    if (n > 0 && s[0] == '/')
        return true;
    const char *colon = memchr(s, ':', n);
    if (!colon || memchr(s, '/', n))
        return false;
    /* "host:port" is a host, "scheme:..." is handled by the caller. */
    if (au_url_looks_like_host(s, n))
        return false;
    return colon > s;
}

char *au_url_from_input(const char *input, const char *search_template, bool *is_search)
{
    if (is_search)
        *is_search = false;
    const char *s = input;
    size_t n = strlen(s);
    while (n && au_is_space((unsigned char)*s))
        s++, n--;
    while (n && au_is_space((unsigned char)s[n - 1]))
        n--;
    if (n == 0)
        return NULL;

    size_t k = au_url_scheme_len(s, n);
    if (k && (known_scheme(s, k) || (k + 2 < n && s[k + 1] == '/' && s[k + 2] == '/'))) {
        /* Lowercase the scheme; leave the rest as typed. */
        char *u = au_strndup(s, n);
        for (size_t i = 0; i < k; i++)
            u[i] = (char)au_lower((unsigned char)u[i]);
        return u;
    }

    bool spaces = false;
    for (size_t i = 0; i < n; i++)
        if (au_is_space((unsigned char)s[i]))
            spaces = true;

    if (is_local_path(s, n)) {
        char *tmp = au_strndup(s, n);
        char *u = au_url_from_path(tmp, s[0] == '/' ? '/' : ':');
        au_free(tmp);
        return u;
    }

    if (!spaces) {
        size_t hn = 0;
        while (hn < n && s[hn] != '/' && s[hn] != '?' && s[hn] != '#')
            hn++;
        if (au_url_looks_like_host(s, hn)) {
            AuBuf b;
            au_buf_init(&b);
            au_buf_puts(&b, "http://");
            au_buf_append(&b, s, n);
            if (hn == n)
                au_buf_putc(&b, '/');
            return au_buf_take(&b, NULL);
        }
    }

    if (is_search)
        *is_search = true;
    const char *t = search_template && *search_template ? search_template : "https://duckduckgo.com/html/?q=%s";
    AuBuf b;
    au_buf_init(&b);
    const char *mark = strstr(t, "%s");
    if (mark) {
        au_buf_append(&b, t, (size_t)(mark - t));
        au_url_form_encode(&b, s, n);
        au_buf_puts(&b, mark + 2);
    } else {
        au_buf_puts(&b, t);
        au_url_form_encode(&b, s, n);
    }
    return au_buf_take(&b, NULL);
}
