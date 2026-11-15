/* The CORS protocol's rules (see cors.h). */
#include "cors.h"
#include "psl.h"
#include "../url/url.h"

#include <stdlib.h>
#include <string.h>

static bool ieq(const char *a, const char *b)
{
    return au_ieq(a, strlen(a), b, strlen(b));
}

static bool in_list(const char *name, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (ieq(name, list[i]))
            return true;
    return false;
}

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

bool au_cors_forbidden_method(const char *m)
{
    return ieq(m, "CONNECT") || ieq(m, "TRACE") || ieq(m, "TRACK");
}

bool au_cors_safelisted_method(const char *m)
{
    return !strcmp(m, "GET") || !strcmp(m, "HEAD") || !strcmp(m, "POST");
}

bool au_cors_forbidden_request_header(const char *name, const char *value)
{
    static const char *const f[] = {"accept-charset", "accept-encoding", "access-control-request-headers",
                                    "access-control-request-method", "connection", "content-length", "cookie",
                                    "cookie2", "date", "dnt", "expect", "host", "keep-alive", "origin", "referer",
                                    "set-cookie", "te", "trailer", "transfer-encoding", "upgrade", "via"};
    if (in_list(name, f, COUNT(f)))
        return true;
    size_t n = strlen(name);
    if ((n >= 6 && au_ieq(name, 6, "proxy-", 6)) || (n >= 4 && au_ieq(name, 4, "sec-", 4)))
        return true;
    if (ieq(name, "x-http-method") || ieq(name, "x-http-method-override") || ieq(name, "x-method-override")) {
        /* a comma-separated list of methods: none may be forbidden */
        const char *p = value ? value : "";
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == ',')
                p++;
            size_t k = strcspn(p, ", \t");
            if ((k == 7 && au_ieq(p, 7, "CONNECT", 7)) || (k == 5 && au_ieq(p, 5, "TRACE", 5)) ||
                (k == 5 && au_ieq(p, 5, "TRACK", 5)))
                return true;
            p += k;
        }
    }
    return false;
}

/* A byte that makes a header value unsafe for the safelist. */
static bool unsafe_byte(unsigned char c)
{
    return (c < 0x20 && c != '\t') || c == '"' || c == '(' || c == ')' || c == ':' || c == '<' || c == '>' ||
           c == '?' || c == '@' || c == '[' || c == '\\' || c == ']' || c == '{' || c == '}' || c == 0x7f;
}

static bool safelisted(const char *name, const char *value, bool range_too)
{
    size_t vn = strlen(value);
    if (vn > 128)
        return false;
    if (ieq(name, "accept")) {
        for (size_t i = 0; i < vn; i++)
            if (unsafe_byte((unsigned char)value[i]))
                return false;
        return true;
    }
    if (ieq(name, "accept-language") || ieq(name, "content-language")) {
        for (size_t i = 0; i < vn; i++) {
            unsigned char c = (unsigned char)value[i];
            bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == ' ' ||
                      c == '*' || c == ',' || c == '-' || c == '.' || c == ';' || c == '=';
            if (!ok)
                return false;
        }
        return true;
    }
    if (ieq(name, "content-type")) {
        for (size_t i = 0; i < vn; i++)
            if (unsafe_byte((unsigned char)value[i]))
                return false;
        size_t e = strcspn(value, ";");
        while (e && (value[e - 1] == ' ' || value[e - 1] == '\t'))
            e--;
        const char *v = value;
        while (e && (*v == ' ' || *v == '\t')) {
            v++;
            e--;
        }
        return au_ieq(v, e, "application/x-www-form-urlencoded", 33) || au_ieq(v, e, "multipart/form-data", 19) ||
               au_ieq(v, e, "text/plain", 10);
    }
    if (range_too && ieq(name, "range")) {
        /* bytes=N- or bytes=N-M */
        if (strncmp(value, "bytes=", 6) != 0)
            return false;
        const char *p = value + 6;
        if (*p < '0' || *p > '9')
            return false;
        while (*p >= '0' && *p <= '9')
            p++;
        if (*p++ != '-')
            return false;
        while (*p >= '0' && *p <= '9')
            p++;
        return *p == 0;
    }
    return false;
}

bool au_cors_safelisted_request_header(const char *name, const char *value)
{
    return safelisted(name, value ? value : "", true);
}

bool au_cors_no_cors_header(const char *name, const char *value)
{
    return safelisted(name, value ? value : "", false);
}

bool au_cors_non_wildcard_request_header(const char *name)
{
    return ieq(name, "authorization");
}

bool au_cors_forbidden_response_header(const char *name)
{
    return ieq(name, "set-cookie") || ieq(name, "set-cookie2");
}

const char *au_cors_header(const char *const *headers, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++)
        if (ieq(headers[2 * i], name))
            return headers[2 * i + 1];
    return NULL;
}

/* Whether the comma-separated lists in every header called name hold
 * token (without case), or "*" when star is given. */
static bool list_has(const char *const *headers, size_t n, const char *name, const char *token, bool *star)
{
    bool found = false;
    size_t tn = strlen(token);
    for (size_t i = 0; i < n; i++) {
        if (!ieq(headers[2 * i], name))
            continue;
        const char *p = headers[2 * i + 1];
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == ',')
                p++;
            size_t k = strcspn(p, ",");
            size_t e = k;
            while (e && (p[e - 1] == ' ' || p[e - 1] == '\t'))
                e--;
            if (e == 1 && p[0] == '*' && star)
                *star = true;
            if (au_ieq(p, e, token, tn))
                found = true;
            p += k;
        }
    }
    return found;
}

bool au_cors_exposed_response_header(const char *name, const char *const *headers, size_t n, bool credentials)
{
    static const char *const safe[] = {"cache-control", "content-language", "content-length", "content-type",
                                       "expires", "last-modified", "pragma"};
    if (au_cors_forbidden_response_header(name))
        return false;
    if (in_list(name, safe, COUNT(safe)))
        return true;
    bool star = false;
    bool listed = list_has(headers, n, "access-control-expose-headers", name, &star);
    return listed || (star && !credentials);
}

bool au_cors_check(const char *origin, bool credentials, const char *const *headers, size_t n)
{
    /* More than one Access-Control-Allow-Origin field is a failure: the
     * combined value matches no origin. */
    const char *acao = NULL;
    for (size_t i = 0; i < n; i++)
        if (ieq(headers[2 * i], "access-control-allow-origin")) {
            if (acao)
                return false;
            acao = headers[2 * i + 1];
        }
    if (!acao)
        return false;
    if (!credentials && !strcmp(acao, "*"))
        return true;
    if (strcmp(acao, origin) != 0)
        return false;
    if (!credentials)
        return true;
    const char *acac = au_cors_header(headers, n, "access-control-allow-credentials");
    return acac && !strcmp(acac, "true");
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

bool au_cors_unsafe_header_names(const char *const *rh, size_t n, AuBuf *out)
{
    au_buf_clear(out);
    char **names = n ? au_alloc(n * sizeof *names) : NULL;
    size_t k = 0;
    /* The safelisted headers of one request may hold 1024 bytes of value
     * in all; past that every one of them counts as unsafe. */
    size_t safe_bytes = 0;
    for (size_t i = 0; i < n; i++)
        if (au_cors_safelisted_request_header(rh[2 * i], rh[2 * i + 1]))
            safe_bytes += strlen(rh[2 * i + 1]);
    for (size_t i = 0; i < n; i++) {
        if (au_cors_safelisted_request_header(rh[2 * i], rh[2 * i + 1]) && safe_bytes <= 1024)
            continue;
        size_t len = strlen(rh[2 * i]);
        char *low = au_strndup(rh[2 * i], len);
        for (size_t j = 0; j < len; j++)
            if (low[j] >= 'A' && low[j] <= 'Z')
                low[j] = (char)(low[j] + 32);
        bool dup = false;
        for (size_t j = 0; j < k; j++)
            if (!strcmp(names[j], low))
                dup = true;
        if (dup)
            au_free(low);
        else
            names[k++] = low;
    }
    if (k)
        qsort(names, k, sizeof *names, cmp_str);
    for (size_t j = 0; j < k; j++) {
        if (j)
            au_buf_putc(out, ',');
        au_buf_puts(out, names[j]);
        au_free(names[j]);
    }
    au_free(names);
    return k > 0;
}

/* Whether what a preflight response (or a cache entry's copy of its two
 * lists) allows covers the request. */
static bool allows(bool credentials, const char *method, const char *const *rh, size_t nreq,
                   const char *const *resp, size_t nresp, const char **why)
{
    bool star = false;
    bool listed = list_has(resp, nresp, "access-control-allow-methods", method, &star);
    if (!listed && !au_cors_safelisted_method(method) && !(star && !credentials)) {
        *why = "the method is not in Access-Control-Allow-Methods";
        return false;
    }
    AuBuf names;
    au_buf_init(&names);
    bool ok = true;
    if (au_cors_unsafe_header_names(rh, nreq, &names)) {
        char *p = au_buf_cstr(&names);
        while (*p && ok) {
            size_t k = strcspn(p, ",");
            char save = p[k];
            p[k] = 0;
            star = false;
            listed = list_has(resp, nresp, "access-control-allow-headers", p, &star);
            if (!listed && !(star && !credentials && !au_cors_non_wildcard_request_header(p)))
                ok = false;
            p[k] = save;
            p += k + (save ? 1 : 0);
        }
    }
    au_buf_free(&names);
    if (!ok)
        *why = "a request header is not in Access-Control-Allow-Headers";
    return ok;
}

bool au_cors_preflight_allows(const char *origin, bool credentials, const char *method, const char *const *rh,
                              size_t nreq, int status, const char *const *resp, size_t nresp, const char **why)
{
    const char *w = "";
    if (!why)
        why = &w;
    if (!au_cors_check(origin, credentials, resp, nresp)) {
        *why = "the preflight response has no matching Access-Control-Allow-Origin";
        return false;
    }
    if (status < 200 || status > 299) {
        *why = "the preflight response's status is not ok";
        return false;
    }
    return allows(credentials, method, rh, nreq, resp, nresp, why);
}

uint32_t au_cors_max_age(const char *const *resp, size_t n)
{
    const char *v = au_cors_header(resp, n, "access-control-max-age");
    if (!v || *v < '0' || *v > '9')
        return 5;
    uint32_t age = 0;
    for (; *v >= '0' && *v <= '9'; v++) {
        age = age * 10 + (uint32_t)(*v - '0');
        if (age > AU_CORS_MAX_AGE)
            return AU_CORS_MAX_AGE;
    }
    return *v ? 5 : age;
}

/* ---- The preflight cache ---- */

typedef struct CorsEntry {
    char *origin, *url;
    bool credentials;
    char *methods, *headers; /* the two Access-Control-Allow lists, as sent (joined with commas) */
    double expires;
    uint32_t seq;
} CorsEntry;

struct AuCorsCache {
    CorsEntry e[AU_CORS_CACHE_MAX];
    int n;
    uint32_t seq;
};

AuCorsCache *au_cors_cache_new(void)
{
    return au_calloc(sizeof(AuCorsCache));
}

static void entry_free(CorsEntry *e)
{
    au_free(e->origin);
    au_free(e->url);
    au_free(e->methods);
    au_free(e->headers);
}

void au_cors_cache_clear(AuCorsCache *c)
{
    for (int i = 0; i < c->n; i++)
        entry_free(&c->e[i]);
    c->n = 0;
}

void au_cors_cache_free(AuCorsCache *c)
{
    if (!c)
        return;
    au_cors_cache_clear(c);
    au_free(c);
}

static char *join_values(const char *const *h, size_t n, const char *name)
{
    AuBuf b;
    au_buf_init(&b);
    for (size_t i = 0; i < n; i++)
        if (ieq(h[2 * i], name)) {
            if (b.len)
                au_buf_putc(&b, ',');
            au_buf_puts(&b, h[2 * i + 1]);
        }
    au_buf_putc(&b, 0);
    size_t len;
    return au_buf_take(&b, &len);
}

static CorsEntry *cache_find(AuCorsCache *c, const char *origin, const char *url, bool credentials, double now)
{
    for (int i = 0; i < c->n;) {
        if (c->e[i].expires <= now) {
            entry_free(&c->e[i]);
            c->e[i] = c->e[--c->n];
            continue;
        }
        i++;
    }
    for (int i = 0; i < c->n; i++)
        if (c->e[i].credentials == credentials && !strcmp(c->e[i].origin, origin) && !strcmp(c->e[i].url, url))
            return &c->e[i];
    return NULL;
}

void au_cors_cache_store(AuCorsCache *c, const char *origin, const char *url, bool credentials,
                         const char *const *resp, size_t nresp, double now)
{
    uint32_t age = au_cors_max_age(resp, nresp);
    CorsEntry *e = cache_find(c, origin, url, credentials, now);
    if (!age) {
        if (e) {
            entry_free(e);
            *e = c->e[--c->n];
        }
        return;
    }
    if (e) {
        au_free(e->methods);
        au_free(e->headers);
    } else {
        if (c->n == AU_CORS_CACHE_MAX) {
            int old = 0;
            for (int i = 1; i < c->n; i++)
                if (c->e[i].seq < c->e[old].seq)
                    old = i;
            entry_free(&c->e[old]);
            c->e[old] = c->e[--c->n];
        }
        e = &c->e[c->n++];
        e->origin = au_strndup(origin, strlen(origin));
        e->url = au_strndup(url, strlen(url));
        e->credentials = credentials;
    }
    e->methods = join_values(resp, nresp, "access-control-allow-methods");
    e->headers = join_values(resp, nresp, "access-control-allow-headers");
    e->expires = now + (double)age * 1000.0;
    e->seq = ++c->seq;
}

bool au_cors_cache_allows(AuCorsCache *c, const char *origin, const char *url, bool credentials, const char *method,
                          const char *const *rh, size_t nreq, double now)
{
    CorsEntry *e = cache_find(c, origin, url, credentials, now);
    if (!e)
        return false;
    const char *resp[4] = {"access-control-allow-methods", e->methods, "access-control-allow-headers", e->headers};
    const char *why;
    return allows(credentials, method, rh, nreq, resp, 2, &why);
}

/* ---- Origins ---- */

void au_cors_origin_of(const char *url, AuBuf *out)
{
    au_buf_clear(out);
    AuUrl u;
    if (!url || !au_url_parse(&u, url, NULL)) {
        au_buf_puts(out, "null");
        return;
    }
    if (!strcmp(u.scheme, "file")) {
        au_buf_puts(out, "file://");
    } else {
        AuOrigin o;
        au_url_origin(&u, &o);
        au_origin_serialize(&o, out);
        au_origin_free(&o);
    }
    au_url_free(&u);
}

bool au_cors_same_origin(const char *a, const char *b)
{
    AuBuf oa, ob;
    au_buf_init(&oa);
    au_buf_init(&ob);
    au_cors_origin_of(a, &oa);
    au_cors_origin_of(b, &ob);
    bool same = strcmp(au_buf_cstr(&oa), "null") != 0 && !strcmp(au_buf_cstr(&oa), au_buf_cstr(&ob));
    au_buf_free(&oa);
    au_buf_free(&ob);
    return same;
}

bool au_corp_allows(const char *corp, const char *origin, const char *url)
{
    if (!corp)
        return true;
    while (*corp == ' ' || *corp == '\t')
        corp++;
    size_t n = strcspn(corp, " \t;,");
    bool same_origin = n == 11 && !strncmp(corp, "same-origin", 11);
    bool same_site = n == 9 && !strncmp(corp, "same-site", 9);
    if (!same_origin && !same_site)
        return true; /* cross-origin, or a value Aura does not know */
    AuBuf o;
    au_buf_init(&o);
    au_cors_origin_of(url, &o);
    bool same = strcmp(origin, "null") != 0 && !strcmp(au_buf_cstr(&o), origin);
    au_buf_free(&o);
    if (same || same_origin)
        return same;
    /* same-site: the registrable domains agree, and a secure response is
     * not handed to an insecure document. */
    AuUrl a, b;
    if (!au_url_parse(&a, origin, NULL))
        return false;
    if (!au_url_parse(&b, url, NULL)) {
        au_url_free(&a);
        return false;
    }
    bool ok = au_same_registrable(a.host, b.host) && !(!strcmp(b.scheme, "https") && strcmp(a.scheme, "https") != 0);
    au_url_free(&a);
    au_url_free(&b);
    return ok;
}
