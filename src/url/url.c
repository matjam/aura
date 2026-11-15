/* The WHATWG URL parser (basic URL parser without state override) and the
 * URL serializer. The state names follow the standard. */
#include "url_int.h"

#include <stdio.h>
#include <string.h>

/* ---- Percent-encoding ---- */

bool au_pct_in_set(unsigned c, AuPctSet set)
{
    if (c < 0x20 || c > 0x7E)
        return true;
    switch (set) {
    case AU_PCT_C0:
        return false;
    case AU_PCT_FRAGMENT:
        return c == ' ' || c == '"' || c == '<' || c == '>' || c == '`';
    case AU_PCT_QUERY:
        return c == ' ' || c == '"' || c == '#' || c == '<' || c == '>';
    case AU_PCT_SPECIAL_QUERY:
        return c == ' ' || c == '"' || c == '#' || c == '<' || c == '>' || c == '\'';
    case AU_PCT_PATH:
        return au_pct_in_set(c, AU_PCT_QUERY) || c == '?' || c == '^' || c == '`' || c == '{' || c == '}';
    case AU_PCT_USERINFO:
        return au_pct_in_set(c, AU_PCT_PATH) || c == '/' || c == ':' || c == ';' || c == '=' || c == '@' ||
               (c >= '[' && c <= ']') || c == '|';
    case AU_PCT_COMPONENT:
        return au_pct_in_set(c, AU_PCT_USERINFO) || (c >= '$' && c <= '&') || c == '+' || c == ',';
    case AU_PCT_FORM:
        return au_pct_in_set(c, AU_PCT_COMPONENT) || c == '!' || (c >= '\'' && c <= ')') || c == '~';
    }
    return true;
}

void au_pct_encode(AuBuf *out, const char *s, size_t n, AuPctSet set)
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned c = (unsigned char)s[i];
        if (au_pct_in_set(c, set)) {
            char e[3] = {'%', hex[c >> 4], hex[c & 15]};
            au_buf_append(out, e, 3);
        } else {
            au_buf_putc(out, (char)c);
        }
    }
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

void au_pct_decode(AuBuf *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n && hexval((unsigned char)s[i + 1]) >= 0 &&
            hexval((unsigned char)s[i + 2]) >= 0) {
            au_buf_putc(out, (char)(hexval((unsigned char)s[i + 1]) * 16 + hexval((unsigned char)s[i + 2])));
            i += 2;
        } else {
            au_buf_putc(out, s[i]);
        }
    }
}

/* ---- Schemes ---- */

int au_url_default_port(const char *scheme)
{
    if (!strcmp(scheme, "http") || !strcmp(scheme, "ws"))
        return 80;
    if (!strcmp(scheme, "https") || !strcmp(scheme, "wss"))
        return 443;
    if (!strcmp(scheme, "ftp"))
        return 21;
    return -1;
}

static bool is_special(const char *scheme)
{
    return !strcmp(scheme, "http") || !strcmp(scheme, "https") || !strcmp(scheme, "ws") ||
           !strcmp(scheme, "wss") || !strcmp(scheme, "ftp") || !strcmp(scheme, "file");
}

/* ---- The record being built ---- */

typedef struct Seg {
    char *s;
    size_t n;
} Seg;

typedef struct Rec {
    AuBuf scheme, username, password, host, opaque, query, fragment;
    bool has_host, has_query, has_fragment, opaque_path;
    AuUrlHostType host_type;
    uint32_t ipv4;
    uint16_t ipv6[8];
    int port;
    Seg *path;
    size_t npath, cappath;
} Rec;

static void rec_init(Rec *r)
{
    memset(r, 0, sizeof *r);
    r->port = -1;
}

static void path_clear(Rec *r)
{
    for (size_t i = 0; i < r->npath; i++)
        au_free(r->path[i].s);
    r->npath = 0;
}

static void rec_free(Rec *r)
{
    au_buf_free(&r->scheme);
    au_buf_free(&r->username);
    au_buf_free(&r->password);
    au_buf_free(&r->host);
    au_buf_free(&r->opaque);
    au_buf_free(&r->query);
    au_buf_free(&r->fragment);
    path_clear(r);
    au_free(r->path);
}

static void path_push(Rec *r, const char *s, size_t n)
{
    if (r->npath == r->cappath) {
        r->cappath = r->cappath ? r->cappath * 2 : 8;
        r->path = au_realloc(r->path, r->cappath * sizeof *r->path);
    }
    r->path[r->npath].s = au_strndup(s, n);
    r->path[r->npath].n = n;
    r->npath++;
}

static void buf_set(AuBuf *b, const char *s, size_t n)
{
    au_buf_clear(b);
    au_buf_append(b, s, n);
}

static bool buf_eq(const AuBuf *b, const char *s)
{
    size_t n = strlen(s);
    return b->len == n && (n == 0 || !memcmp(b->data, s, n));
}

static const char *cstr(const char *s) { return s ? s : ""; }

/* Copies host, credentials and port from a parsed URL. */
static void copy_authority(Rec *r, const AuUrl *b)
{
    buf_set(&r->username, b->username, strlen(b->username));
    buf_set(&r->password, b->password, strlen(b->password));
    r->has_host = b->host_type != AU_URL_HOST_NULL;
    buf_set(&r->host, b->host, strlen(b->host));
    r->host_type = b->host_type;
    r->ipv4 = b->ipv4;
    memcpy(r->ipv6, b->ipv6, sizeof r->ipv6);
    r->port = b->port;
}

static void copy_path(Rec *r, const AuUrl *b)
{
    path_clear(r);
    if (b->opaque_path) {
        r->opaque_path = true;
        buf_set(&r->opaque, b->path, strlen(b->path));
        return;
    }
    /* b->path is "/seg/seg"; split it back into segments. */
    const char *p = b->path;
    while (*p == '/') {
        const char *q = strchr(p + 1, '/');
        size_t n = q ? (size_t)(q - p - 1) : strlen(p + 1);
        path_push(r, p + 1, n);
        if (!q)
            break;
        p = q;
    }
}

static void copy_query(Rec *r, const AuUrl *b)
{
    r->has_query = b->query != NULL;
    buf_set(&r->query, cstr(b->query), strlen(cstr(b->query)));
}

static bool is_drive(const char *s, size_t n, bool normalized)
{
    return n == 2 && au_is_alpha((unsigned char)s[0]) && (s[1] == ':' || (!normalized && s[1] == '|'));
}

/* "starts with a Windows drive letter" */
static bool starts_with_drive(const char *s, size_t n)
{
    return n >= 2 && is_drive(s, 2, false) &&
           (n == 2 || s[2] == '/' || s[2] == '\\' || s[2] == '?' || s[2] == '#');
}

static void shorten_path(Rec *r)
{
    if (buf_eq(&r->scheme, "file") && r->npath == 1 && is_drive(r->path[0].s, r->path[0].n, true))
        return;
    if (r->npath) {
        au_free(r->path[r->npath - 1].s);
        r->npath--;
    }
}

static bool single_dot(const AuBuf *b)
{
    return buf_eq(b, ".") || (b->len == 3 && au_ieq(b->data, 3, "%2e", 3));
}

static bool double_dot(const AuBuf *b)
{
    if (b->len == 2)
        return !memcmp(b->data, "..", 2);
    if (b->len == 4)
        return au_ieq(b->data, 4, ".%2e", 4) || au_ieq(b->data, 4, "%2e.", 4);
    if (b->len == 6)
        return au_ieq(b->data, 6, "%2e%2e", 6);
    return false;
}

static bool parse_host_into(Rec *r, const char *s, size_t n, bool special)
{
    AuBuf h;
    au_buf_init(&h);
    AuUrlHostType t;
    if (!url_host_parse(s, n, special, &h, &t, &r->ipv4, r->ipv6)) {
        au_buf_free(&h);
        return false;
    }
    buf_set(&r->host, h.data ? h.data : "", h.len);
    r->host_type = t;
    r->has_host = true;
    au_buf_free(&h);
    return true;
}

enum {
    ST_SCHEME_START,
    ST_SCHEME,
    ST_NO_SCHEME,
    ST_SPECIAL_RELATIVE_OR_AUTHORITY,
    ST_PATH_OR_AUTHORITY,
    ST_RELATIVE,
    ST_RELATIVE_SLASH,
    ST_SPECIAL_AUTHORITY_SLASHES,
    ST_SPECIAL_AUTHORITY_IGNORE_SLASHES,
    ST_AUTHORITY,
    ST_HOST,
    ST_PORT,
    ST_FILE,
    ST_FILE_SLASH,
    ST_FILE_HOST,
    ST_PATH_START,
    ST_PATH,
    ST_OPAQUE_PATH,
    ST_QUERY,
    ST_FRAGMENT
};

/* Builds the AuUrl from the record. */
static void finish(AuUrl *u, Rec *r)
{
    memset(u, 0, sizeof *u);
    u->scheme = au_strndup(cstr(r->scheme.data), r->scheme.len);
    u->username = au_strndup(cstr(r->username.data), r->username.len);
    u->password = au_strndup(cstr(r->password.data), r->password.len);
    u->host_type = r->has_host ? r->host_type : AU_URL_HOST_NULL;
    if (u->host_type == AU_URL_HOST_NULL)
        u->host = au_strndup("", 0);
    else
        u->host = au_strndup(cstr(r->host.data), r->host.len);
    if (r->has_host && r->host.len == 0)
        u->host_type = AU_URL_HOST_EMPTY;
    u->ipv4 = r->ipv4;
    memcpy(u->ipv6, r->ipv6, sizeof u->ipv6);
    u->port = r->port;
    u->special = is_special(u->scheme);
    u->opaque_path = r->opaque_path;
    AuBuf p;
    au_buf_init(&p);
    if (r->opaque_path) {
        au_buf_append(&p, cstr(r->opaque.data), r->opaque.len);
    } else {
        for (size_t i = 0; i < r->npath; i++) {
            au_buf_putc(&p, '/');
            au_buf_append(&p, r->path[i].s, r->path[i].n);
        }
    }
    au_buf_cstr(&p);
    u->path = au_buf_take(&p, NULL);
    u->query = r->has_query ? au_strndup(cstr(r->query.data), r->query.len) : NULL;
    u->fragment = r->has_fragment ? au_strndup(cstr(r->fragment.data), r->fragment.len) : NULL;
    /* href */
    AuBuf h;
    au_buf_init(&h);
    au_buf_puts(&h, u->scheme);
    au_buf_putc(&h, ':');
    if (u->host_type != AU_URL_HOST_NULL) {
        au_buf_puts(&h, "//");
        if (u->username[0] || u->password[0]) {
            au_buf_puts(&h, u->username);
            if (u->password[0]) {
                au_buf_putc(&h, ':');
                au_buf_puts(&h, u->password);
            }
            au_buf_putc(&h, '@');
        }
        au_buf_puts(&h, u->host);
        if (u->port >= 0)
            au_buf_printf(&h, ":%d", u->port);
    } else if (!u->opaque_path && r->npath > 1 && r->path[0].n == 0) {
        au_buf_puts(&h, "/.");
    }
    au_buf_puts(&h, u->path);
    if (u->query) {
        au_buf_putc(&h, '?');
        au_buf_puts(&h, u->query);
    }
    if (u->fragment) {
        au_buf_putc(&h, '#');
        au_buf_puts(&h, u->fragment);
    }
    au_buf_cstr(&h);
    u->href = au_buf_take(&h, NULL);
}

/* Cleans the input: valid UTF-8 (invalid sequences become U+FFFD),
 * leading and trailing C0 controls and spaces stripped, tabs and newlines
 * removed. */
static void clean_input(const char *in, size_t n, AuBuf *out)
{
    size_t a = 0, b = n;
    while (a < b && (unsigned char)in[a] <= 0x20)
        a++;
    while (b > a && (unsigned char)in[b - 1] <= 0x20)
        b--;
    for (size_t i = a; i < b;) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) {
            if (c != '\t' && c != '\n' && c != '\r')
                au_buf_putc(out, (char)c);
            i++;
            continue;
        }
        size_t adv;
        uint32_t cp = au_utf8_decode(in + i, b - i, &adv);
        au_buf_put_utf8(out, cp);
        i += adv;
    }
}

bool au_url_parse_n(AuUrl *u, const char *input, size_t len, const AuUrl *base)
{
    memset(u, 0, sizeof *u);
    if (len > AU_URL_MAX_INPUT)
        return false;
    AuBuf inb, buf;
    au_buf_init(&inb);
    au_buf_init(&buf);
    clean_input(input, len, &inb);
    const char *s = inb.data ? inb.data : "";
    size_t n = inb.len;
    Rec r;
    rec_init(&r);
    int state = ST_SCHEME_START;
    bool at_seen = false, in_brackets = false, pw_seen = false, ok = true;
    bool special = false;
    /* p may step to -1 (one before the start); positions are signed. */
    long p = 0;
    for (;; p++) {
        int c = (p >= 0 && (size_t)p < n) ? (unsigned char)s[p] : -1; /* -1 is EOF */
        const char *rest = (p >= 0 && (size_t)p < n) ? s + p + 1 : s + n;
        size_t restn = (p >= 0 && (size_t)p < n) ? n - (size_t)p - 1 : 0;
        switch (state) {
        case ST_SCHEME_START:
            if (c != -1 && au_is_alpha(c)) {
                au_buf_putc(&buf, (char)au_lower(c));
                state = ST_SCHEME;
            } else {
                state = ST_NO_SCHEME;
                p--;
            }
            break;
        case ST_SCHEME:
            if (c != -1 && (au_is_alpha(c) || au_is_digit(c) || c == '+' || c == '-' || c == '.')) {
                au_buf_putc(&buf, (char)au_lower(c));
            } else if (c == ':') {
                buf_set(&r.scheme, cstr(buf.data), buf.len);
                au_buf_cstr(&r.scheme);
                special = is_special(r.scheme.data);
                au_buf_clear(&buf);
                if (buf_eq(&r.scheme, "file")) {
                    state = ST_FILE;
                } else if (special && base && !strcmp(base->scheme, r.scheme.data)) {
                    state = ST_SPECIAL_RELATIVE_OR_AUTHORITY;
                } else if (special) {
                    state = ST_SPECIAL_AUTHORITY_SLASHES;
                } else if (restn && rest[0] == '/') {
                    state = ST_PATH_OR_AUTHORITY;
                    p++;
                } else {
                    r.opaque_path = true;
                    au_buf_clear(&r.opaque);
                    state = ST_OPAQUE_PATH;
                }
            } else {
                au_buf_clear(&buf);
                state = ST_NO_SCHEME;
                p = -1;
            }
            break;
        case ST_NO_SCHEME:
            if (!base || (base->opaque_path && c != '#')) {
                ok = false;
            } else if (base->opaque_path && c == '#') {
                buf_set(&r.scheme, base->scheme, strlen(base->scheme));
                au_buf_cstr(&r.scheme);
                special = is_special(r.scheme.data);
                copy_path(&r, base);
                copy_query(&r, base);
                r.has_fragment = true;
                au_buf_clear(&r.fragment);
                state = ST_FRAGMENT;
            } else if (strcmp(base->scheme, "file") != 0) {
                state = ST_RELATIVE;
                p--;
            } else {
                state = ST_FILE;
                p--;
            }
            break;
        case ST_SPECIAL_RELATIVE_OR_AUTHORITY:
            if (c == '/' && restn && rest[0] == '/') {
                state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
                p++;
            } else {
                state = ST_RELATIVE;
                p--;
            }
            break;
        case ST_PATH_OR_AUTHORITY:
            if (c == '/') {
                state = ST_AUTHORITY;
            } else {
                state = ST_PATH;
                p--;
            }
            break;
        case ST_RELATIVE:
            buf_set(&r.scheme, base->scheme, strlen(base->scheme));
            au_buf_cstr(&r.scheme);
            special = is_special(r.scheme.data);
            if (c == '/') {
                state = ST_RELATIVE_SLASH;
            } else if (special && c == '\\') {
                state = ST_RELATIVE_SLASH;
            } else {
                copy_authority(&r, base);
                copy_path(&r, base);
                copy_query(&r, base);
                if (c == '?') {
                    r.has_query = true;
                    au_buf_clear(&r.query);
                    state = ST_QUERY;
                } else if (c == '#') {
                    r.has_fragment = true;
                    au_buf_clear(&r.fragment);
                    state = ST_FRAGMENT;
                } else if (c != -1) {
                    r.has_query = false;
                    au_buf_clear(&r.query);
                    shorten_path(&r);
                    state = ST_PATH;
                    p--;
                }
            }
            break;
        case ST_RELATIVE_SLASH:
            if (special && (c == '/' || c == '\\')) {
                state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
            } else if (c == '/') {
                state = ST_AUTHORITY;
            } else {
                copy_authority(&r, base);
                state = ST_PATH;
                p--;
            }
            break;
        case ST_SPECIAL_AUTHORITY_SLASHES:
            if (c == '/' && restn && rest[0] == '/') {
                state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
                p++;
            } else {
                state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
                p--;
            }
            break;
        case ST_SPECIAL_AUTHORITY_IGNORE_SLASHES:
            if (c != '/' && c != '\\') {
                state = ST_AUTHORITY;
                p--;
            }
            break;
        case ST_AUTHORITY:
            if (c == '@') {
                if (at_seen) {
                    AuBuf t;
                    au_buf_init(&t);
                    au_buf_puts(&t, "%40");
                    au_buf_append(&t, cstr(buf.data), buf.len);
                    au_buf_free(&buf);
                    buf = t;
                }
                at_seen = true;
                for (size_t i = 0; i < buf.len; i++) {
                    if (buf.data[i] == ':' && !pw_seen) {
                        pw_seen = true;
                        continue;
                    }
                    au_pct_encode(pw_seen ? &r.password : &r.username, buf.data + i, 1, AU_PCT_USERINFO);
                }
                au_buf_clear(&buf);
            } else if (c == -1 || c == '/' || c == '?' || c == '#' || (special && c == '\\')) {
                if (at_seen && buf.len == 0) {
                    ok = false;
                    break;
                }
                p -= (long)buf.len + 1;
                au_buf_clear(&buf);
                state = ST_HOST;
            } else {
                au_buf_putc(&buf, (char)c);
            }
            break;
        case ST_HOST:
            if (c == ':' && !in_brackets) {
                if (buf.len == 0) {
                    ok = false;
                    break;
                }
                if (!parse_host_into(&r, cstr(buf.data), buf.len, special)) {
                    ok = false;
                    break;
                }
                au_buf_clear(&buf);
                state = ST_PORT;
            } else if (c == -1 || c == '/' || c == '?' || c == '#' || (special && c == '\\')) {
                p--;
                if (special && buf.len == 0) {
                    ok = false;
                    break;
                }
                if (!parse_host_into(&r, cstr(buf.data), buf.len, special)) {
                    ok = false;
                    break;
                }
                au_buf_clear(&buf);
                state = ST_PATH_START;
            } else {
                if (c == '[')
                    in_brackets = true;
                if (c == ']')
                    in_brackets = false;
                au_buf_putc(&buf, (char)c);
            }
            break;
        case ST_PORT:
            if (c != -1 && au_is_digit(c)) {
                au_buf_putc(&buf, (char)c);
            } else if (c == -1 || c == '/' || c == '?' || c == '#' || (special && c == '\\')) {
                if (buf.len) {
                    long port = 0;
                    for (size_t i = 0; i < buf.len; i++) {
                        port = port * 10 + (buf.data[i] - '0');
                        if (port > 65535)
                            break;
                    }
                    if (port > 65535) {
                        ok = false;
                        break;
                    }
                    au_buf_cstr(&r.scheme);
                    r.port = port == au_url_default_port(r.scheme.data) ? -1 : (int)port;
                    au_buf_clear(&buf);
                }
                state = ST_PATH_START;
                p--;
            } else {
                ok = false;
            }
            break;
        case ST_FILE:
            buf_set(&r.scheme, "file", 4);
            au_buf_cstr(&r.scheme);
            special = true;
            r.has_host = true;
            r.host_type = AU_URL_HOST_EMPTY;
            au_buf_clear(&r.host);
            if (c == '/' || c == '\\') {
                state = ST_FILE_SLASH;
            } else if (base && !strcmp(base->scheme, "file")) {
                copy_authority(&r, base);
                copy_path(&r, base);
                copy_query(&r, base);
                if (c == '?') {
                    r.has_query = true;
                    au_buf_clear(&r.query);
                    state = ST_QUERY;
                } else if (c == '#') {
                    r.has_fragment = true;
                    au_buf_clear(&r.fragment);
                    state = ST_FRAGMENT;
                } else if (c != -1) {
                    r.has_query = false;
                    au_buf_clear(&r.query);
                    if (!starts_with_drive(s + p, n - (size_t)p))
                        shorten_path(&r);
                    else
                        path_clear(&r);
                    state = ST_PATH;
                    p--;
                }
            } else {
                state = ST_PATH;
                p--;
            }
            break;
        case ST_FILE_SLASH:
            if (c == '/' || c == '\\') {
                state = ST_FILE_HOST;
            } else {
                if (base && !strcmp(base->scheme, "file")) {
                    r.has_host = true;
                    buf_set(&r.host, base->host, strlen(base->host));
                    r.host_type = base->host_type == AU_URL_HOST_NULL ? AU_URL_HOST_EMPTY : base->host_type;
                    if (!starts_with_drive(c == -1 ? s + n : s + p, c == -1 ? 0 : n - (size_t)p)) {
                        /* base's first path segment, if a normalized drive */
                        const char *bp = base->path;
                        if (!base->opaque_path && bp[0] == '/' && strlen(bp) >= 3 && is_drive(bp + 1, 2, true) &&
                            (bp[3] == 0 || bp[3] == '/'))
                            path_push(&r, bp + 1, 2);
                    }
                }
                state = ST_PATH;
                p--;
            }
            break;
        case ST_FILE_HOST:
            if (c == -1 || c == '/' || c == '\\' || c == '?' || c == '#') {
                p--;
                if (is_drive(cstr(buf.data), buf.len, false)) {
                    state = ST_PATH; /* the buffer is kept: it is the first segment */
                } else if (buf.len == 0) {
                    r.has_host = true;
                    au_buf_clear(&r.host);
                    r.host_type = AU_URL_HOST_EMPTY;
                    state = ST_PATH_START;
                } else {
                    if (!parse_host_into(&r, cstr(buf.data), buf.len, true)) {
                        ok = false;
                        break;
                    }
                    if (buf_eq(&r.host, "localhost")) {
                        au_buf_clear(&r.host);
                        r.host_type = AU_URL_HOST_EMPTY;
                    }
                    au_buf_clear(&buf);
                    state = ST_PATH_START;
                }
            } else {
                au_buf_putc(&buf, (char)c);
            }
            break;
        case ST_PATH_START:
            if (special) {
                state = ST_PATH;
                if (c != '/' && c != '\\')
                    p--;
            } else if (c == '?') {
                r.has_query = true;
                au_buf_clear(&r.query);
                state = ST_QUERY;
            } else if (c == '#') {
                r.has_fragment = true;
                au_buf_clear(&r.fragment);
                state = ST_FRAGMENT;
            } else if (c != -1) {
                state = ST_PATH;
                if (c != '/')
                    p--;
            }
            break;
        case ST_PATH:
            if (c == -1 || c == '/' || (special && c == '\\') || c == '?' || c == '#') {
                bool slash = c == '/' || (special && c == '\\');
                if (double_dot(&buf)) {
                    shorten_path(&r);
                    if (!slash)
                        path_push(&r, "", 0);
                } else if (single_dot(&buf) && !slash) {
                    path_push(&r, "", 0);
                } else if (!single_dot(&buf)) {
                    if (buf_eq(&r.scheme, "file") && r.npath == 0 && is_drive(cstr(buf.data), buf.len, false))
                        buf.data[1] = ':';
                    path_push(&r, cstr(buf.data), buf.len);
                }
                au_buf_clear(&buf);
                if (c == '?') {
                    r.has_query = true;
                    au_buf_clear(&r.query);
                    state = ST_QUERY;
                } else if (c == '#') {
                    r.has_fragment = true;
                    au_buf_clear(&r.fragment);
                    state = ST_FRAGMENT;
                }
            } else {
                char ch = (char)c;
                au_pct_encode(&buf, &ch, 1, AU_PCT_PATH);
            }
            break;
        case ST_OPAQUE_PATH:
            if (c == '?') {
                r.has_query = true;
                au_buf_clear(&r.query);
                state = ST_QUERY;
            } else if (c == '#') {
                r.has_fragment = true;
                au_buf_clear(&r.fragment);
                state = ST_FRAGMENT;
            } else if (c == ' ') {
                if (restn && (rest[0] == '?' || rest[0] == '#'))
                    au_buf_puts(&r.opaque, "%20");
                else
                    au_buf_putc(&r.opaque, ' ');
            } else if (c != -1) {
                char ch = (char)c;
                au_pct_encode(&r.opaque, &ch, 1, AU_PCT_C0);
            }
            break;
        case ST_QUERY:
            if (c == '#' || c == -1) {
                au_pct_encode(&r.query, cstr(buf.data), buf.len, special ? AU_PCT_SPECIAL_QUERY : AU_PCT_QUERY);
                au_buf_clear(&buf);
                if (c == '#') {
                    r.has_fragment = true;
                    au_buf_clear(&r.fragment);
                    state = ST_FRAGMENT;
                }
            } else {
                au_buf_putc(&buf, (char)c);
            }
            break;
        case ST_FRAGMENT:
            if (c != -1) {
                char ch = (char)c;
                au_pct_encode(&r.fragment, &ch, 1, AU_PCT_FRAGMENT);
            }
            break;
        }
        if (!ok || (c == -1 && p >= (long)n))
            break;
    }
    if (ok)
        finish(u, &r);
    rec_free(&r);
    au_buf_free(&buf);
    au_buf_free(&inb);
    return ok;
}

bool au_url_parse(AuUrl *u, const char *input, const AuUrl *base)
{
    return au_url_parse_n(u, input, strlen(input), base);
}

void au_url_free(AuUrl *u)
{
    if (!u)
        return;
    au_free(u->href);
    au_free(u->scheme);
    au_free(u->username);
    au_free(u->password);
    au_free(u->host);
    au_free(u->path);
    au_free(u->query);
    au_free(u->fragment);
    memset(u, 0, sizeof *u);
}

static char *dup_or_null(const char *s)
{
    return s ? au_strndup(s, strlen(s)) : NULL;
}

void au_url_copy(AuUrl *dst, const AuUrl *src)
{
    *dst = *src;
    dst->href = dup_or_null(src->href);
    dst->scheme = dup_or_null(src->scheme);
    dst->username = dup_or_null(src->username);
    dst->password = dup_or_null(src->password);
    dst->host = dup_or_null(src->host);
    dst->path = dup_or_null(src->path);
    dst->query = dup_or_null(src->query);
    dst->fragment = dup_or_null(src->fragment);
}

char *au_url_resolve_str(const char *base, const char *rel)
{
    AuUrl b, u;
    bool have_base = false;
    if (base && base[0]) {
        if (!au_url_parse(&b, base, NULL))
            return NULL;
        have_base = true;
    }
    char *out = NULL;
    if (au_url_parse(&u, rel, have_base ? &b : NULL)) {
        out = au_strndup(u.href, strlen(u.href));
        au_url_free(&u);
    }
    if (have_base)
        au_url_free(&b);
    return out;
}

int au_url_effective_port(const AuUrl *u)
{
    return u->port >= 0 ? u->port : au_url_default_port(u->scheme);
}

void au_url_serialize_no_fragment(const AuUrl *u, AuBuf *out)
{
    au_buf_clear(out);
    const char *h = u->href;
    size_t n = strlen(h);
    if (u->fragment)
        n -= strlen(u->fragment) + 1;
    au_buf_append(out, h, n);
}

bool au_url_equal_no_fragment(const AuUrl *a, const AuUrl *b)
{
    size_t na = strlen(a->href) - (a->fragment ? strlen(a->fragment) + 1 : 0);
    size_t nb = strlen(b->href) - (b->fragment ? strlen(b->fragment) + 1 : 0);
    return na == nb && !memcmp(a->href, b->href, na);
}

/* ---- Origins ---- */

void au_url_origin(const AuUrl *u, AuOrigin *o)
{
    memset(o, 0, sizeof *o);
    o->opaque = true;
    o->port = -1;
    if (!strcmp(u->scheme, "blob")) {
        AuUrl inner;
        if (au_url_parse(&inner, u->path, NULL)) {
            if (!strcmp(inner.scheme, "http") || !strcmp(inner.scheme, "https"))
                au_url_origin(&inner, o);
            au_url_free(&inner);
        }
        return;
    }
    if (strcmp(u->scheme, "http") && strcmp(u->scheme, "https") && strcmp(u->scheme, "ws") &&
        strcmp(u->scheme, "wss") && strcmp(u->scheme, "ftp"))
        return;
    o->opaque = false;
    snprintf(o->scheme, sizeof o->scheme, "%s", u->scheme);
    o->host = au_strndup(u->host, strlen(u->host));
    o->port = au_url_effective_port(u);
}

void au_origin_free(AuOrigin *o)
{
    au_free(o->host);
    o->host = NULL;
}

bool au_origin_same(const AuOrigin *a, const AuOrigin *b)
{
    if (a->opaque || b->opaque)
        return false;
    return !strcmp(a->scheme, b->scheme) && !strcmp(a->host, b->host) && a->port == b->port;
}

void au_origin_serialize(const AuOrigin *o, AuBuf *out)
{
    if (o->opaque) {
        au_buf_puts(out, "null");
        return;
    }
    au_buf_printf(out, "%s://%s", o->scheme, o->host);
    if (o->port >= 0 && o->port != au_url_default_port(o->scheme))
        au_buf_printf(out, ":%d", o->port);
}
