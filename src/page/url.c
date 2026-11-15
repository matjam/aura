/* Relative URL resolution: a limited subset of the WHATWG URL Standard
 * (see page.h). */
#include "page.h"

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

/* Appends path to out (at *len) removing dot segments. */
static void remove_dots(char *path, size_t *plen)
{
    char *src = path, *end = path + *plen;
    char out[4096];
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
                while (o > 0 && out[o - 1] != '/')
                    o--;
            }
        } else {
            if (o + n + 1 >= sizeof out)
                break;
            memcpy(out + o, src, n);
            o += n;
            if (slash)
                out[o++] = '/';
        }
        src = seg_end + (slash ? 1 : 0);
        if (!slash)
            break;
    }
    if ((end - path >= 2 && end[-1] == '.' && end[-2] == '/') ||
        (end - path >= 3 && end[-1] == '.' && end[-2] == '.' && end[-3] == '/')) {
        /* a trailing "." or ".." leaves a directory */
        if (o == 0 || out[o - 1] != '/')
            out[o++] = '/';
    }
    memcpy(path, out, o);
    *plen = o;
}

bool au_url_resolve(const char *base, const char *rel, char *out, size_t cap)
{
    char r[4096];
    size_t rn = 0;
    if (!rel)
        rel = "";
    /* Trim C0 controls and spaces; drop tabs and newlines inside. */
    while (*rel && (unsigned char)*rel <= ' ')
        rel++;
    for (const char *p = rel; *p && rn < sizeof r - 1; p++) {
        if (*p == '\t' || *p == '\n' || *p == '\r')
            continue;
        r[rn++] = *p == '\\' ? '/' : *p;
    }
    while (rn && (unsigned char)r[rn - 1] <= ' ')
        rn--;
    r[rn] = 0;
    if (!base)
        base = "";
    size_t bl = strlen(base);
    size_t rs = scheme_len(r, rn);
    char buf[8192];
    size_t n = 0;
#define PUT(s, len) do { if (n + (len) >= sizeof buf) return false; memcpy(buf + n, (s), (len)); n += (len); } while (0)
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
                /* Dot segments in the path part (before ? or #). */
                size_t pend = pstart;
                while (pend < n && buf[pend] != '?' && buf[pend] != '#')
                    pend++;
                size_t plen = pend - pstart;
                char tail[4096];
                size_t tl = n - pend;
                if (tl >= sizeof tail)
                    return false;
                memcpy(tail, buf + pend, tl);
                if (plen < 4096) {
                    remove_dots(buf + pstart, &plen);
                }
                n = pstart + plen;
                PUT(tail, tl);
            }
        }
    }
#undef PUT
    if (n + 1 > cap)
        return false;
    memcpy(out, buf, n);
    out[n] = 0;
    return true;
}
