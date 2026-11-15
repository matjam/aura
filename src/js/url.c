/* A minimal URL parser for URL, URLSearchParams, location and anchors.
 *
 * MARKED FOR REPLACEMENT: the WHATWG URL parser from the networking work
 * (src/url) replaces this at merge; js_url_parse is the one entry point to
 * swap. It handles absolute URLs, relative references against a base
 * (scheme-relative, path-absolute, path-relative with dot segments,
 * query-only, fragment-only), lowercases scheme and host, drops default
 * ports, turns backslashes into slashes for special schemes and
 * percent-encodes spaces, controls and non-ASCII bytes. It does not do
 * IDNA, IPv4/IPv6 host parsing or full validation. */
#include "js_int.h"

#include <string.h>

static bool is_special(const char *scheme)
{
    static const char *const s[] = {"http", "https", "ftp", "ws", "wss", "file"};
    for (size_t i = 0; i < JS_COUNTOF(s); i++)
        if (!strcmp(scheme, s[i]))
            return true;
    return false;
}

static const char *default_port(const char *scheme)
{
    if (!strcmp(scheme, "http") || !strcmp(scheme, "ws"))
        return "80";
    if (!strcmp(scheme, "https") || !strcmp(scheme, "wss"))
        return "443";
    if (!strcmp(scheme, "ftp"))
        return "21";
    return NULL;
}

/* Length of the scheme at the start of s (with the colon at s[len]), or 0. */
static size_t scheme_len(const char *s)
{
    if (!au_is_alpha((unsigned char)s[0]))
        return 0;
    size_t i = 1;
    while (au_is_alpha((unsigned char)s[i]) || au_is_digit((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' ||
           s[i] == '.')
        i++;
    return s[i] == ':' ? i : 0;
}

static void enc(AuBuf *b, const char *s, size_t n, const char *extra)
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '`' || (extra && strchr(extra, c))) {
            au_buf_putc(b, '%');
            au_buf_putc(b, hex[c >> 4]);
            au_buf_putc(b, hex[c & 15]);
        } else {
            au_buf_putc(b, (char)c);
        }
    }
}

typedef struct Parts {
    char scheme[32];
    AuBuf user, pass, host, port, path, query, frag;
    bool has_auth, has_query, has_frag, opaque;
} Parts;

static void parts_init(Parts *p)
{
    memset(p, 0, sizeof *p);
    au_buf_init(&p->user);
    au_buf_init(&p->pass);
    au_buf_init(&p->host);
    au_buf_init(&p->port);
    au_buf_init(&p->path);
    au_buf_init(&p->query);
    au_buf_init(&p->frag);
}

static void parts_free(Parts *p)
{
    au_buf_free(&p->user);
    au_buf_free(&p->pass);
    au_buf_free(&p->host);
    au_buf_free(&p->port);
    au_buf_free(&p->path);
    au_buf_free(&p->query);
    au_buf_free(&p->frag);
}

/* Removes dot segments from a path that starts with '/'. */
static void remove_dots(AuBuf *path)
{
    AuBuf out;
    au_buf_init(&out);
    const char *s = path->data ? path->data : "";
    size_t n = path->len, i = 0;
    while (i < n) {
        size_t st = i + 1, e = st;
        while (e < n && s[e] != '/')
            e++;
        size_t sl = e - st;
        const char *seg = s + st;
        bool last = e >= n;
        if ((sl == 1 && seg[0] == '.') || (sl == 3 && (!memcmp(seg, "%2e", 3) || !memcmp(seg, "%2E", 3)))) {
            if (last)
                au_buf_putc(&out, '/');
        } else if ((sl == 2 && !memcmp(seg, "..", 2)) || (sl == 4 && au_ieq(seg, 4, ".%2e", 4)) ||
                   (sl == 4 && au_ieq(seg, 4, "%2e.", 4)) || (sl == 6 && au_ieq(seg, 6, "%2e%2e", 6))) {
            while (out.len && out.data[out.len - 1] != '/')
                out.len--;
            if (out.len)
                out.len--;
            if (last)
                au_buf_putc(&out, '/');
        } else {
            au_buf_putc(&out, '/');
            au_buf_append(&out, seg, sl);
        }
        i = e;
    }
    if (!out.len)
        au_buf_putc(&out, '/');
    au_buf_free(path);
    *path = out;
}

/* Parses an absolute URL string into parts. */
static bool parse_abs(const char *in, Parts *p)
{
    size_t sl = scheme_len(in);
    if (!sl || sl >= sizeof p->scheme)
        return false;
    for (size_t i = 0; i < sl; i++)
        p->scheme[i] = (char)au_lower((unsigned char)in[i]);
    p->scheme[sl] = 0;
    const char *s = in + sl + 1;
    bool special = is_special(p->scheme);
    /* fragment and query first */
    const char *hash = strchr(s, '#');
    size_t end = hash ? (size_t)(hash - s) : strlen(s);
    if (hash) {
        p->has_frag = true;
        enc(&p->frag, hash + 1, strlen(hash + 1), NULL);
    }
    const char *q = memchr(s, '?', end);
    size_t pend = q ? (size_t)(q - s) : end;
    if (q) {
        p->has_query = true;
        enc(&p->query, q + 1, end - (size_t)(q + 1 - s), special ? "'" : NULL);
    }
    char *rest = au_strndup(s, pend);
    if (special)
        for (char *c = rest; *c; c++)
            if (*c == '\\')
                *c = '/';
    const char *r = rest;
    if (r[0] == '/' && r[1] == '/') {
        p->has_auth = true;
        r += 2;
        const char *pe = strchr(r, '/');
        size_t al = pe ? (size_t)(pe - r) : strlen(r);
        const char *at = NULL;
        for (size_t i = 0; i < al; i++)
            if (r[i] == '@')
                at = r + i;
        const char *h = r;
        if (at) {
            const char *colon = memchr(r, ':', (size_t)(at - r));
            if (colon) {
                enc(&p->user, r, (size_t)(colon - r), "@/:");
                enc(&p->pass, colon + 1, (size_t)(at - colon - 1), "@/:");
            } else {
                enc(&p->user, r, (size_t)(at - r), "@/:");
            }
            h = at + 1;
        }
        size_t hl = al - (size_t)(h - r);
        const char *colon = NULL;
        if (h[0] != '[') {
            colon = memchr(h, ':', hl);
        } else {
            const char *rb = memchr(h, ']', hl);
            if (rb && (size_t)(rb - h) + 1 < hl && rb[1] == ':')
                colon = rb + 1;
        }
        size_t hostlen = colon ? (size_t)(colon - h) : hl;
        for (size_t i = 0; i < hostlen; i++) {
            unsigned char c = (unsigned char)h[i];
            if (c == ' ' || c == '<' || c == '>' || c == '^' || c == '|') {
                au_free(rest);
                return false;
            }
            au_buf_putc(&p->host, (char)au_lower(c));
        }
        if (special && !hostlen && strcmp(p->scheme, "file") != 0) {
            au_free(rest);
            return false;
        }
        if (colon) {
            const char *pp = colon + 1;
            size_t pl = hl - hostlen - 1;
            long port = 0;
            for (size_t i = 0; i < pl; i++) {
                if (!au_is_digit((unsigned char)pp[i]) || port > 65535) {
                    au_free(rest);
                    return false;
                }
                port = port * 10 + (pp[i] - '0');
            }
            if (pl && port <= 65535) {
                const char *dp = default_port(p->scheme);
                char buf[8];
                int bl = 0;
                long v = port;
                char tmp[8];
                do {
                    tmp[bl++] = (char)('0' + v % 10);
                    v /= 10;
                } while (v);
                for (int i = 0; i < bl; i++)
                    buf[i] = tmp[bl - 1 - i];
                buf[bl] = 0;
                if (!dp || strcmp(dp, buf) != 0)
                    au_buf_puts(&p->port, buf);
            }
        }
        r += al;
    } else if (special && strcmp(p->scheme, "file") != 0) {
        /* "http:example.com" without slashes: treat the rest as authority. */
        p->has_auth = true;
        const char *pe = strchr(r, '/');
        size_t hl = pe ? (size_t)(pe - r) : strlen(r);
        for (size_t i = 0; i < hl; i++)
            au_buf_putc(&p->host, (char)au_lower((unsigned char)r[i]));
        if (!hl) {
            au_free(rest);
            return false;
        }
        r += hl;
    }
    if (special || p->has_auth) {
        if (r[0] != '/')
            au_buf_putc(&p->path, '/');
        enc(&p->path, r, strlen(r), NULL);
        remove_dots(&p->path);
    } else {
        p->opaque = r[0] != '/';
        enc(&p->path, r, strlen(r), NULL);
        if (!p->opaque)
            remove_dots(&p->path);
    }
    au_free(rest);
    return true;
}

static void serialize(const Parts *p, AuBuf *out)
{
    au_buf_puts(out, p->scheme);
    au_buf_putc(out, ':');
    if (p->has_auth) {
        au_buf_puts(out, "//");
        if (p->user.len || p->pass.len) {
            au_buf_append(out, p->user.data, p->user.len);
            if (p->pass.len) {
                au_buf_putc(out, ':');
                au_buf_append(out, p->pass.data, p->pass.len);
            }
            au_buf_putc(out, '@');
        }
        au_buf_append(out, p->host.data, p->host.len);
        if (p->port.len) {
            au_buf_putc(out, ':');
            au_buf_append(out, p->port.data, p->port.len);
        }
    }
    au_buf_append(out, p->path.data, p->path.len);
    if (p->has_query) {
        au_buf_putc(out, '?');
        au_buf_append(out, p->query.data, p->query.len);
    }
    if (p->has_frag) {
        au_buf_putc(out, '#');
        au_buf_append(out, p->frag.data, p->frag.len);
    }
}

/* Resolves a relative reference against a parsed base into an absolute
 * URL string. */
static char *resolve(const char *rel, const char *base_str)
{
    Parts b;
    parts_init(&b);
    if (!parse_abs(base_str, &b)) {
        parts_free(&b);
        return NULL;
    }
    AuBuf out;
    au_buf_init(&out);
    bool special = is_special(b.scheme);
    char *r = au_strndup(rel, strlen(rel));
    if (special)
        for (char *c = r; *c; c++)
            if (*c == '\\')
                *c = '/';
    if (b.opaque && r[0] != '#') {
        au_free(r);
        parts_free(&b);
        return NULL;
    }
    if (r[0] == '/' && r[1] == '/') {
        au_buf_printf(&out, "%s:%s", b.scheme, r);
    } else {
        Parts t = b;
        AuBuf path;
        au_buf_init(&path);
        AuBuf base_out;
        au_buf_init(&base_out);
        /* rebuild from components */
        au_buf_puts(&out, b.scheme);
        au_buf_putc(&out, ':');
        if (b.has_auth) {
            au_buf_puts(&out, "//");
            if (b.user.len || b.pass.len) {
                au_buf_append(&out, b.user.data, b.user.len);
                if (b.pass.len) {
                    au_buf_putc(&out, ':');
                    au_buf_append(&out, b.pass.data, b.pass.len);
                }
                au_buf_putc(&out, '@');
            }
            au_buf_append(&out, b.host.data, b.host.len);
            if (b.port.len) {
                au_buf_putc(&out, ':');
                au_buf_append(&out, b.port.data, b.port.len);
            }
        }
        if (r[0] == '/') {
            au_buf_puts(&out, r);
        } else if (r[0] == '?') {
            au_buf_append(&out, b.path.data, b.path.len);
            au_buf_puts(&out, r);
        } else if (r[0] == '#') {
            au_buf_append(&out, b.path.data, b.path.len);
            if (b.has_query) {
                au_buf_putc(&out, '?');
                au_buf_append(&out, b.query.data, b.query.len);
            }
            au_buf_puts(&out, r);
        } else if (r[0] == 0) {
            au_buf_append(&out, b.path.data, b.path.len);
            if (b.has_query) {
                au_buf_putc(&out, '?');
                au_buf_append(&out, b.query.data, b.query.len);
            }
        } else {
            /* merge: base path up to its last slash, then rel */
            size_t keep = 0;
            for (size_t i = 0; i < b.path.len; i++)
                if (b.path.data[i] == '/')
                    keep = i + 1;
            if (!keep)
                au_buf_putc(&out, '/');
            au_buf_append(&out, b.path.data, keep);
            au_buf_puts(&out, r);
        }
        (void)t;
        au_buf_free(&path);
        au_buf_free(&base_out);
    }
    au_free(r);
    parts_free(&b);
    return au_buf_take(&out, NULL);
}

static char *dupb(const AuBuf *b, const char *prefix)
{
    AuBuf o;
    au_buf_init(&o);
    if (prefix)
        au_buf_puts(&o, prefix);
    au_buf_append(&o, b->data ? b->data : "", b->len);
    return au_buf_take(&o, NULL);
}

/* "http:foo" against an http base is relative (same special scheme). */
static bool is_special_prefix(const char *in, size_t sl, const char *base)
{
    if (scheme_len(base) != sl)
        return false;
    char sch[32];
    for (size_t i = 0; i < sl; i++) {
        sch[i] = (char)au_lower((unsigned char)in[i]);
        if (sch[i] != au_lower((unsigned char)base[i]))
            return false;
    }
    sch[sl] = 0;
    return is_special(sch);
}

bool js_url_parse(const char *input, const char *base, JsUrl *out)
{
    memset(out, 0, sizeof *out);
    /* trim C0 control or space */
    while (*input && (unsigned char)*input <= 0x20)
        input++;
    size_t n = strlen(input);
    while (n && (unsigned char)input[n - 1] <= 0x20)
        n--;
    char *in = au_alloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (input[i] != '\t' && input[i] != '\n' && input[i] != '\r')
            in[k++] = input[i];
    in[k] = 0;
    char *abs = NULL;
    size_t sl = scheme_len(in);
    if (sl && !(base && in[sl + 1] != '/' && sl < 32 && is_special_prefix(in, sl, base))) {
        abs = in;
        in = NULL;
    } else if (base) {
        abs = resolve(sl ? in + sl + 1 : in, base);
    }
    au_free(in);
    if (!abs)
        return false;
    Parts p;
    parts_init(&p);
    bool ok = parse_abs(abs, &p);
    au_free(abs);
    if (!ok) {
        parts_free(&p);
        return false;
    }
    AuBuf h;
    au_buf_init(&h);
    serialize(&p, &h);
    out->href = au_buf_take(&h, NULL);
    AuBuf t;
    au_buf_init(&t);
    au_buf_puts(&t, p.scheme);
    au_buf_putc(&t, ':');
    out->protocol = au_buf_take(&t, NULL);
    out->username = dupb(&p.user, NULL);
    out->password = dupb(&p.pass, NULL);
    out->hostname = dupb(&p.host, NULL);
    out->port = dupb(&p.port, NULL);
    out->pathname = dupb(&p.path, NULL);
    out->search = p.has_query && p.query.len ? dupb(&p.query, "?") : au_strndup("", 0);
    out->hash = p.has_frag && p.frag.len ? dupb(&p.frag, "#") : au_strndup("", 0);
    parts_free(&p);
    return true;
}

void js_url_free(JsUrl *u)
{
    au_free(u->href);
    au_free(u->protocol);
    au_free(u->username);
    au_free(u->password);
    au_free(u->hostname);
    au_free(u->port);
    au_free(u->pathname);
    au_free(u->search);
    au_free(u->hash);
    memset(u, 0, sizeof *u);
}
