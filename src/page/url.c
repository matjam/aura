/* Relative URL resolution: a limited subset of the WHATWG URL Standard
 * (see page.h). Any length is resolved: a data: URL of an image or a font
 * is often tens of kilobytes. */
#include "page.h"
#include "../url/url.h"

#include <string.h>

static size_t scheme_len(const char *s, size_t n)
{
    if (!n || !au_is_alpha((unsigned char)s[0]))
        return 0;
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        if (c == ':')
            return i;
        if (!(au_is_alpha((unsigned char)c) || au_is_digit((unsigned char)c) || c == '+' || c == '-' || c == '.'))
            return 0;
    }
    return 0;
}

/* Removes dot segments from path (*plen bytes), in place: each kept
 * segment moves to an offset no later than where it was read. */
static void remove_dots(char *path, size_t *plen)
{
    char *src = path, *end = path + *plen;
    bool trailing = (end - path >= 2 && end[-1] == '.' && end[-2] == '/') ||
                    (end - path >= 3 && end[-1] == '.' && end[-2] == '.' && end[-3] == '/');
    size_t o = 0;
    while (src < end) {
        char *seg_end = memchr(src, '/', (size_t)(end - src));
        if (!seg_end)
            seg_end = end;
        size_t n = (size_t)(seg_end - src);
        bool slash = seg_end < end;
        if (n == 1 && src[0] == '.') {
            /* skip */
        } else if (n == 2 && src[0] == '.' && src[1] == '.') {
            /* drop the last segment of out (keep the leading '/') */
            if (o > 1) {
                o--; /* the slash ending the previous segment */
                while (o > 0 && path[o - 1] != '/')
                    o--;
            }
        } else {
            memmove(path + o, src, n);
            o += n;
            if (slash)
                path[o++] = '/';
        }
        src = seg_end + (slash ? 1 : 0);
        if (!slash)
            break;
    }
    /* a trailing "." or ".." leaves a directory */
    if (trailing && (o == 0 || path[o - 1] != '/'))
        path[o++] = '/';
    *plen = o;
}

/* Resolves rel against base. The result is NUL-terminated in work when
 * work (wcap bytes) holds what resolution needs, else in a block from
 * au_alloc that the caller frees; *len is its length. NULL when an input
 * is longer than AU_URL_MAX_INPUT. */
static char *resolve(const char *base, const char *rel, char *work, size_t wcap, size_t *len)
{
    if (!rel)
        rel = "";
    if (!base)
        base = "";
    /* Trim C0 controls and spaces; drop tabs and newlines inside. */
    while (*rel && (unsigned char)*rel <= ' ')
        rel++;
    size_t rl = strlen(rel), bl = strlen(base);
    if (rl > AU_URL_MAX_INPUT || bl > AU_URL_MAX_INPUT)
        return NULL;
    /* The result (at most the base, a slash and rel) and the cleaned rel
     * share one buffer. */
    size_t bcap = bl + rl + 2, need = bcap + rl + 1;
    char *buf = need <= wcap ? work : au_alloc(need);
    char *r = buf + bcap;
    size_t rn = 0;
    for (const char *p = rel; *p; p++) {
        if (*p == '\t' || *p == '\n' || *p == '\r')
            continue;
        r[rn++] = *p == '\\' ? '/' : *p;
    }
    while (rn && (unsigned char)r[rn - 1] <= ' ')
        rn--;
    r[rn] = 0;
    size_t rs = scheme_len(r, rn);
    size_t n = 0;
#define PUT(s, l)                                                                                                      \
    do {                                                                                                               \
        memcpy(buf + n, (s), (l));                                                                                     \
        n += (l);                                                                                                      \
    } while (0)
    if (rs) {
        PUT(r, rn);
    } else {
        size_t bs = scheme_len(base, bl);
        if (!bs) {
            PUT(r, rn);
        } else {
            /* Split the base: scheme ':' ['//' authority] path ['?' query] ['#' frag] */
            const char *auth = base + bs + 1;
            const char *path = auth;
            bool has_auth = auth[0] == '/' && auth[1] == '/';
            if (has_auth) {
                path = auth + 2;
                while (*path && *path != '/' && *path != '?' && *path != '#')
                    path++;
            }
            const char *q = path;
            while (*q && *q != '?' && *q != '#')
                q++;
            const char *h = q;
            while (*h && *h != '#')
                h++;
            if (rn >= 2 && r[0] == '/' && r[1] == '/') {
                PUT(base, bs + 1);
                PUT(r, rn);
            } else if (rn && r[0] == '#') {
                PUT(base, (size_t)(h - base));
                PUT(r, rn);
            } else if (rn == 0) {
                PUT(base, (size_t)(h - base));
            } else if (r[0] == '?') {
                PUT(base, (size_t)(q - base));
                PUT(r, rn);
            } else {
                PUT(base, (size_t)(path - base));
                size_t pstart = n;
                if (r[0] == '/') {
                    PUT(r, rn);
                } else {
                    /* merge: the base path up to its last '/' */
                    const char *last = NULL;
                    for (const char *p = path; p < q; p++)
                        if (*p == '/')
                            last = p;
                    if (last)
                        PUT(path, (size_t)(last - path + 1));
                    else if (has_auth)
                        PUT("/", 1);
                    PUT(r, rn);
                }
                /* Dot segments in the path part (before ? or #); the
                 * query and fragment move down after it. */
                size_t pend = pstart;
                while (pend < n && buf[pend] != '?' && buf[pend] != '#')
                    pend++;
                size_t plen = pend - pstart, tl = n - pend;
                remove_dots(buf + pstart, &plen);
                memmove(buf + pstart + plen, buf + pend, tl);
                n = pstart + plen + tl;
            }
        }
    }
#undef PUT
    buf[n] = 0;
    *len = n;
    return buf;
}

bool au_url_resolve(const char *base, const char *rel, char *out, size_t cap)
{
    char work[2048];
    size_t n;
    char *s = resolve(base, rel, work, sizeof work, &n);
    if (!s)
        return false;
    bool ok = n + 1 <= cap;
    if (ok)
        memcpy(out, s, n + 1);
    if (s != work)
        au_free(s);
    return ok;
}

char *au_url_resolve_dup(const char *base, const char *rel)
{
    char work[2048];
    size_t n;
    char *s = resolve(base, rel, work, sizeof work, &n);
    if (!s || s != work)
        return s;
    char *d = au_alloc(n + 1);
    memcpy(d, s, n + 1);
    return d;
}
