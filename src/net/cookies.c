/* The cookie store. See cookies.h. */
#include "cookies.h"
#include "../browser/history.h" /* au_put_i64 */

#include "policy.h"
#include "psl.h"

#include <stdio.h>
#include <string.h>

#define MAX_NAME_VALUE 4096
#define MAX_ATTR 1024

typedef struct CookieRuleEntry {
    char *site;
    AuCookieRule rule;
} CookieRuleEntry;

struct AuCookieJar {
    AuCookie *items;
    size_t n, cap;
    AuNetCookiePolicy policy;
    bool block_third_party;
    bool dirty;
    CookieRuleEntry *rules;
    size_t nrules;
};

AuCookieJar *au_cookie_jar_new(void)
{
    AuCookieJar *j = au_calloc(sizeof *j);
    j->policy = AU_COOKIE_POLICY_ALL;
    j->block_third_party = true;
    return j;
}

static void cookie_free(AuCookie *c)
{
    au_free(c->name);
    au_free(c->value);
    au_free(c->domain);
    au_free(c->path);
    au_free(c->partition);
}

void au_cookie_jar_free(AuCookieJar *j)
{
    if (!j)
        return;
    for (size_t i = 0; i < j->n; i++)
        cookie_free(&j->items[i]);
    au_free(j->items);
    for (size_t i = 0; i < j->nrules; i++)
        au_free(j->rules[i].site);
    au_free(j->rules);
    au_free(j);
}

void au_cookie_jar_set_policy(AuCookieJar *j, AuNetCookiePolicy policy, bool block_third_party)
{
    j->policy = policy;
    j->block_third_party = block_third_party;
}

static void remove_at(AuCookieJar *j, size_t i)
{
    if (j->items[i].expires)
        j->dirty = true;
    cookie_free(&j->items[i]);
    memmove(j->items + i, j->items + i + 1, (j->n - i - 1) * sizeof *j->items);
    j->n--;
}

/* ---- Matching ---- */

/* RFC 6265 5.1.3: host equals domain, or ends with "." domain and is not
 * an IP address. */
static bool domain_match(const char *host, const char *domain)
{
    size_t hn = strlen(host), dn = strlen(domain);
    if (hn == dn)
        return !memcmp(host, domain, dn);
    if (hn < dn + 1 || host[hn - dn - 1] != '.' || memcmp(host + hn - dn, domain, dn))
        return false;
    return !(hn && (au_is_digit((unsigned char)host[hn - 1]) || host[0] == '['));
}

/* RFC 6265 5.1.4 */
static bool path_match(const char *req, const char *cookie)
{
    size_t cn = strlen(cookie), rn = strlen(req);
    if (rn < cn || memcmp(req, cookie, cn))
        return false;
    return rn == cn || cookie[cn - 1] == '/' || req[cn] == '/';
}

static void default_path(const AuUrl *u, AuBuf *out)
{
    au_buf_clear(out);
    const char *p = u->opaque_path ? "" : u->path;
    const char *last = strrchr(p, '/');
    if (!p[0] || p[0] != '/' || last == p)
        au_buf_putc(out, '/');
    else
        au_buf_append(out, p, (size_t)(last - p));
    au_buf_cstr(out);
}

static bool is_secure_url(const AuUrl *u)
{
    return au_url_trustworthy(u) && strcmp(u->scheme, "file") != 0;
}

/* The top-level site, scheme and registrable domain. */
static void top_site(const AuCookieRequest *rq, AuBuf *out)
{
    au_url_site(rq->site_for_cookies ? rq->site_for_cookies : rq->url, out);
}

bool au_cookie_is_third_party(const AuCookieRequest *rq)
{
    if (!rq->site_for_cookies)
        return false;
    return !au_same_registrable(rq->site_for_cookies->host, rq->url->host);
}

/* RFC 6265bis "same-site" for SameSite enforcement: schemeful same site
 * with the site for cookies, and for navigations with the initiator. */
static bool same_site_request(const AuCookieRequest *rq)
{
    if (rq->site_for_cookies && !au_same_site(rq->site_for_cookies, rq->url))
        return false;
    if (rq->initiator && !au_same_site(rq->initiator, rq->url))
        return false;
    return true;
}

/* ---- Set-Cookie parsing ---- */

static bool ctl_in(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c < 0x20 && c != '\t') || c == 0x7F)
            return true;
    }
    return false;
}

static void trim(const char **s, size_t *n)
{
    while (*n && (**s == ' ' || **s == '\t')) {
        (*s)++;
        (*n)--;
    }
    while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t'))
        (*n)--;
}

static bool prefix_ci(const char *s, const char *p)
{
    size_t n = strlen(p);
    return strlen(s) >= n && au_ieq(s, n, p, n);
}

static AuCookie *find_same(AuCookieJar *j, const AuCookie *c)
{
    for (size_t i = 0; i < j->n; i++) {
        AuCookie *o = &j->items[i];
        if (!strcmp(o->name, c->name) && !strcmp(o->domain, c->domain) && !strcmp(o->path, c->path) &&
            o->host_only == c->host_only && !strcmp(o->partition, c->partition))
            return o;
    }
    return NULL;
}

static void registrable_of(const char *host, AuBuf *out)
{
    au_buf_clear(out);
    const char *d;
    size_t dl;
    if (au_psl_registrable(host, &d, &dl))
        au_buf_append(out, d, dl);
    else
        au_buf_puts(out, host);
    au_buf_cstr(out);
}

static void evict(AuCookieJar *j, const char *domain, int64_t now)
{
    /* Expired first. */
    for (size_t i = 0; i < j->n;) {
        if (j->items[i].expires && j->items[i].expires <= now)
            remove_at(j, i);
        else
            i++;
    }
    /* Per registrable domain. */
    AuBuf reg, other;
    au_buf_init(&reg);
    au_buf_init(&other);
    registrable_of(domain, &reg);
    for (;;) {
        size_t count = 0, victim = (size_t)-1;
        for (size_t i = 0; i < j->n; i++) {
            registrable_of(j->items[i].domain, &other);
            if (strcmp(au_buf_cstr(&other), au_buf_cstr(&reg)))
                continue;
            count++;
            if (victim == (size_t)-1 || j->items[i].last_access < j->items[victim].last_access)
                victim = i;
        }
        if (count <= AU_COOKIES_PER_DOMAIN || victim == (size_t)-1)
            break;
        remove_at(j, victim);
    }
    au_buf_free(&reg);
    au_buf_free(&other);
    while (j->n > AU_COOKIES_TOTAL) {
        size_t victim = 0;
        for (size_t i = 1; i < j->n; i++)
            if (j->items[i].last_access < j->items[victim].last_access)
                victim = i;
        remove_at(j, victim);
    }
}

#define IGNORE(reason)                                                                                            \
    do {                                                                                                          \
        if (why)                                                                                                  \
            *why = reason;                                                                                        \
        goto ignore;                                                                                              \
    } while (0)

bool au_cookie_jar_set(AuCookieJar *j, const AuCookieRequest *rq, const char *line, const char **why)
{
    if (why)
        *why = "stored";
    AuCookie c;
    memset(&c, 0, sizeof c);
    AuBuf path, tmp;
    au_buf_init(&path);
    au_buf_init(&tmp);
    const AuUrl *u = rq->url;
    bool partitioned_ok = false;
    if (!u->special || strcmp(u->scheme, "file") == 0 || u->host_type == AU_URL_HOST_NULL || !u->host[0])
        IGNORE("no host");
    AuCookieRule rule = au_cookie_jar_rule(j, u->host);
    if (rule == AU_COOKIE_RULE_BLOCK)
        IGNORE("blocked for this site");
    if (j->policy == AU_COOKIE_POLICY_NONE && rule != AU_COOKIE_RULE_ALLOW)
        IGNORE("cookies are disabled");
    /* name=value */
    const char *semi = strchr(line, ';');
    size_t nvl = semi ? (size_t)(semi - line) : strlen(line);
    const char *eq = memchr(line, '=', nvl);
    const char *name = line, *value;
    size_t nl, vl;
    if (eq) {
        nl = (size_t)(eq - line);
        value = eq + 1;
        vl = nvl - nl - 1;
    } else {
        /* A nameless cookie: the whole pair is the value. */
        nl = 0;
        value = line;
        vl = nvl;
    }
    trim(&name, &nl);
    trim(&value, &vl);
    if (!nl && !vl)
        IGNORE("empty name and value");
    if (nl + vl > MAX_NAME_VALUE)
        IGNORE("too large");
    if (ctl_in(name, nl) || ctl_in(value, vl))
        IGNORE("control character");
    c.name = au_strndup(name, nl);
    c.value = au_strndup(value, vl);
    c.same_site = AU_SAMESITE_DEFAULT;
    int64_t max_age = 0, expires = 0;
    bool have_max_age = false, have_expires = false, have_domain = false, have_path = false;
    AuBuf domain;
    au_buf_init(&domain);
    /* Attributes */
    const char *a = semi;
    while (a && *a == ';') {
        a++;
        const char *next = strchr(a, ';');
        size_t al = next ? (size_t)(next - a) : strlen(a);
        const char *aeq = memchr(a, '=', al);
        const char *an = a, *av = "";
        size_t anl = aeq ? (size_t)(aeq - a) : al, avl = 0;
        if (aeq) {
            av = aeq + 1;
            avl = al - anl - 1;
        }
        trim(&an, &anl);
        trim(&av, &avl);
        if (avl > MAX_ATTR) {
            a = next;
            continue; /* ignore the attribute */
        }
        if (au_ieq(an, anl, "expires", 7)) {
            char tmpd[MAX_ATTR + 1];
            memcpy(tmpd, av, avl);
            tmpd[avl] = 0;
            int64_t t;
            if (au_http_parse_date(tmpd, &t)) {
                expires = t;
                have_expires = true;
            }
        } else if (au_ieq(an, anl, "max-age", 7)) {
            /* digits with an optional leading '-' */
            size_t k = 0;
            bool neg = avl && av[0] == '-';
            if (neg)
                k = 1;
            bool ok = k < avl;
            int64_t v = 0;
            for (; k < avl; k++) {
                if (!au_is_digit((unsigned char)av[k])) {
                    ok = false;
                    break;
                }
                if (v < 100000000000LL)
                    v = v * 10 + (av[k] - '0');
            }
            if (ok) {
                max_age = neg ? -v : v;
                have_max_age = true;
            }
        } else if (au_ieq(an, anl, "domain", 6)) {
            /* The last Domain attribute wins; an empty one is ignored. */
            if (avl) {
                if (av[0] == '.') {
                    av++;
                    avl--;
                }
                au_buf_clear(&domain);
                for (size_t k = 0; k < avl; k++)
                    au_buf_putc(&domain, (char)au_lower((unsigned char)av[k]));
                au_buf_cstr(&domain);
                have_domain = avl > 0;
            }
        } else if (au_ieq(an, anl, "path", 4)) {
            if (avl && av[0] == '/') {
                au_buf_clear(&path);
                au_buf_append(&path, av, avl);
                au_buf_cstr(&path);
                have_path = true;
            } else {
                have_path = false;
            }
        } else if (au_ieq(an, anl, "secure", 6)) {
            c.secure = true;
        } else if (au_ieq(an, anl, "httponly", 8)) {
            c.http_only = true;
        } else if (au_ieq(an, anl, "partitioned", 11)) {
            c.partitioned = true;
        } else if (au_ieq(an, anl, "samesite", 8)) {
            if (au_ieq(av, avl, "none", 4))
                c.same_site = AU_SAMESITE_NONE;
            else if (au_ieq(av, avl, "lax", 3))
                c.same_site = AU_SAMESITE_LAX;
            else if (au_ieq(av, avl, "strict", 6))
                c.same_site = AU_SAMESITE_STRICT;
            else
                c.same_site = AU_SAMESITE_DEFAULT;
        }
        a = next;
    }
    bool secure_origin = is_secure_url(u);
    /* Secure needs a secure origin. */
    if (c.secure && !secure_origin) {
        au_buf_free(&domain);
        IGNORE("Secure cookie from an insecure origin");
    }
    /* Domain */
    if (have_domain) {
        if (au_psl_is_public_suffix(au_buf_cstr(&domain))) {
            if (strcmp(au_buf_cstr(&domain), u->host) == 0) {
                have_domain = false; /* becomes host-only */
            } else {
                au_buf_free(&domain);
                IGNORE("Domain is a public suffix");
            }
        }
    }
    if (have_domain) {
        if (!domain_match(u->host, au_buf_cstr(&domain))) {
            au_buf_free(&domain);
            IGNORE("Domain does not match the host");
        }
        c.domain = au_strndup(domain.data, domain.len);
        c.host_only = false;
    } else {
        c.domain = au_strndup(u->host, strlen(u->host));
        c.host_only = true;
    }
    au_buf_free(&domain);
    if (!have_path)
        default_path(u, &path);
    c.path = au_strndup(path.data, path.len);
    /* Prefixes (case-insensitive). */
    if (prefix_ci(c.name, "__Secure-") && !(c.secure && secure_origin))
        IGNORE("__Secure- prefix without Secure");
    /* __Host- needs an explicit Path=/ (as browsers require), not just a
     * default path of "/". */
    if (prefix_ci(c.name, "__Host-") &&
        !(c.secure && secure_origin && c.host_only && have_path && !strcmp(c.path, "/")))
        IGNORE("__Host- prefix requirements not met");
    /* A nameless cookie whose value looks like a prefixed name is refused. */
    if (!nl && (prefix_ci(c.value, "__Secure-") || prefix_ci(c.value, "__Host-")))
        IGNORE("nameless cookie with a prefix");
    /* HttpOnly cookies cannot be set from script. */
    if (rq->from_script && c.http_only)
        IGNORE("HttpOnly from script");
    /* SameSite=None requires Secure. */
    if (c.same_site == AU_SAMESITE_NONE && !c.secure)
        IGNORE("SameSite=None without Secure");
    /* Third-party policy and partitioning. */
    bool third = au_cookie_is_third_party(rq);
    if (third && rule != AU_COOKIE_RULE_ALLOW && (j->block_third_party || j->policy == AU_COOKIE_POLICY_SAME_SITE)) {
        if (!(c.partitioned && c.secure))
            IGNORE("third-party cookie blocked");
        partitioned_ok = true;
    }
    if (c.partitioned && c.secure) {
        /* CHIPS: a partitioned cookie is keyed by the top-level site,
         * first party or not. */
        top_site(rq, &tmp);
        c.partition = au_strndup(tmp.data, tmp.len);
    } else {
        c.partition = au_strndup("", 0);
        if (c.partitioned && !c.secure)
            c.partitioned = false; /* Partitioned requires Secure; ignored otherwise */
    }
    (void)partitioned_ok;
    /* SameSite Lax/Strict cookies from a cross-site subresource response
     * are not stored. */
    if (c.same_site != AU_SAMESITE_NONE && !same_site_request(rq) && !rq->top_level_nav)
        IGNORE("SameSite cookie from a cross-site request");
    /* Expiry */
    if (have_max_age) {
        c.expires = max_age <= 0 ? 1 : rq->now + max_age;
    } else if (have_expires) {
        c.expires = expires <= rq->now ? 1 : expires;
    }
    if (c.expires > rq->now + AU_COOKIE_MAX_AGE_SECONDS)
        c.expires = rq->now + AU_COOKIE_MAX_AGE_SECONDS;
    if (c.expires < 0)
        c.expires = 1;
    /* Session only: kept until quit; an expiry in the past still deletes. */
    if (rule == AU_COOKIE_RULE_SESSION && c.expires > rq->now)
        c.expires = 0;
    /* Leave secure cookies alone: a non-secure cookie may not overlay a
     * secure one of the same name whose domain matches and path matches. */
    if (!c.secure && !secure_origin) {
        for (size_t i = 0; i < j->n; i++) {
            AuCookie *o = &j->items[i];
            if (o->secure && !strcmp(o->name, c.name) &&
                (domain_match(c.domain, o->domain) || domain_match(o->domain, c.domain)) && path_match(c.path, o->path))
                IGNORE("would overlay a Secure cookie");
        }
    }
    c.creation = c.last_access = rq->now;
    AuCookie *old = find_same(j, &c);
    if (old) {
        if (old->http_only && rq->from_script)
            IGNORE("would replace an HttpOnly cookie from script");
        c.creation = old->creation;
        if (old->expires)
            j->dirty = true;
        size_t idx = (size_t)(old - j->items);
        cookie_free(old);
        j->items[idx] = c;
        if (c.expires && c.expires <= rq->now) {
            remove_at(j, idx);
            j->dirty = true;
        } else if (c.expires) {
            j->dirty = true;
        }
        au_buf_free(&path);
        au_buf_free(&tmp);
        return true;
    }
    if (c.expires && c.expires <= rq->now) {
        /* Expiring a cookie that does not exist: nothing to do. */
        cookie_free(&c);
        au_buf_free(&path);
        au_buf_free(&tmp);
        if (why)
            *why = "already expired";
        return true;
    }
    if (j->n == j->cap) {
        j->cap = j->cap ? j->cap * 2 : 64;
        j->items = au_realloc(j->items, j->cap * sizeof *j->items);
    }
    j->items[j->n++] = c;
    if (c.expires)
        j->dirty = true;
    evict(j, c.domain, rq->now);
    au_buf_free(&path);
    au_buf_free(&tmp);
    return true;
ignore:
    cookie_free(&c);
    au_buf_free(&path);
    au_buf_free(&tmp);
    return false;
}

/* ---- Sending ---- */

typedef struct Pick {
    AuCookie *c;
} Pick;

static int pick_cmp(const AuCookie *a, const AuCookie *b)
{
    size_t pa = strlen(a->path), pb = strlen(b->path);
    if (pa != pb)
        return pa > pb ? -1 : 1;
    return a->creation < b->creation ? -1 : a->creation > b->creation ? 1 : 0;
}

static void collect(AuCookieJar *j, const AuCookieRequest *rq, bool http_api, AuBuf *out)
{
    au_buf_clear(out);
    const AuUrl *u = rq->url;
    if (!u->special || u->host_type == AU_URL_HOST_NULL)
        return;
    AuCookieRule rule = au_cookie_jar_rule(j, u->host);
    if (rule == AU_COOKIE_RULE_BLOCK || (j->policy == AU_COOKIE_POLICY_NONE && rule != AU_COOKIE_RULE_ALLOW))
        return;
    bool third = au_cookie_is_third_party(rq);
    bool block =
        third && rule != AU_COOKIE_RULE_ALLOW && (j->block_third_party || j->policy == AU_COOKIE_POLICY_SAME_SITE);
    bool same = same_site_request(rq);
    bool secure = is_secure_url(u);
    AuBuf site;
    au_buf_init(&site);
    top_site(rq, &site);
    const char *path = u->opaque_path ? "/" : (u->path[0] ? u->path : "/");
    /* Expire as we go. */
    for (size_t i = 0; i < j->n;) {
        if (j->items[i].expires && j->items[i].expires <= rq->now)
            remove_at(j, i);
        else
            i++;
    }
    AuCookie **list = NULL;
    size_t nl = 0;
    for (size_t i = 0; i < j->n; i++) {
        AuCookie *c = &j->items[i];
        if (c->host_only ? strcmp(u->host, c->domain) != 0 : !domain_match(u->host, c->domain))
            continue;
        if (!path_match(path, c->path))
            continue;
        if (c->secure && !secure)
            continue;
        if (c->http_only && !http_api)
            continue;
        if (c->partition[0]) {
            /* Partitioned: only under its top-level site. */
            if (strcmp(c->partition, au_buf_cstr(&site)))
                continue;
        } else if (block) {
            continue;
        }
        if (!same) {
            AuSameSite ss = c->same_site;
            if (ss == AU_SAMESITE_STRICT)
                continue;
            if ((ss == AU_SAMESITE_LAX || ss == AU_SAMESITE_DEFAULT) && !(rq->top_level_nav && rq->safe_method))
                continue;
        }
        list = au_realloc(list, (nl + 1) * sizeof *list);
        list[nl++] = c;
    }
    /* Insertion sort: longer paths first, then older. */
    for (size_t a = 1; a < nl; a++) {
        AuCookie *x = list[a];
        size_t b = a;
        while (b > 0 && pick_cmp(x, list[b - 1]) < 0) {
            list[b] = list[b - 1];
            b--;
        }
        list[b] = x;
    }
    for (size_t k = 0; k < nl; k++) {
        if (k)
            au_buf_puts(out, "; ");
        if (list[k]->name[0]) {
            au_buf_puts(out, list[k]->name);
            au_buf_putc(out, '=');
        }
        au_buf_puts(out, list[k]->value);
        list[k]->last_access = rq->now;
    }
    au_buf_cstr(out);
    au_free(list);
    au_buf_free(&site);
}

bool au_cookie_jar_header(AuCookieJar *j, const AuCookieRequest *rq, AuBuf *out)
{
    collect(j, rq, true, out);
    return out->len > 0;
}

void au_cookie_jar_document_get(AuCookieJar *j, const AuCookieRequest *rq, AuBuf *out)
{
    collect(j, rq, false, out);
}

void au_cookie_jar_document_set(AuCookieJar *j, const AuCookieRequest *rq, const char *value)
{
    AuCookieRequest r = *rq;
    r.from_script = true;
    au_cookie_jar_set(j, &r, value, NULL);
}

void au_cookie_jar_end_session(AuCookieJar *j)
{
    for (size_t i = 0; i < j->n;) {
        if (!j->items[i].expires)
            remove_at(j, i);
        else
            i++;
    }
}

void au_cookie_jar_clear(AuCookieJar *j)
{
    while (j->n)
        remove_at(j, j->n - 1);
    j->dirty = true;
}

size_t au_cookie_jar_count(const AuCookieJar *j) { return j->n; }
const AuCookie *au_cookie_jar_at(const AuCookieJar *j, size_t i) { return i < j->n ? &j->items[i] : NULL; }
void au_cookie_jar_remove_at(AuCookieJar *j, size_t i)
{
    if (i < j->n)
        remove_at(j, i);
}

void au_cookie_jar_remove_domain(AuCookieJar *j, const char *domain)
{
    for (size_t i = 0; i < j->n;) {
        if (domain_match(j->items[i].domain, domain) || !strcmp(j->items[i].domain, domain))
            remove_at(j, i);
        else
            i++;
    }
}

/* ---- Per-site rules ---- */

void au_cookie_site_key(const char *host, AuBuf *out)
{
    au_buf_clear(out);
    if (!host)
        return;
    const char *dom = host;
    size_t len = strlen(host);
    if (len && host[len - 1] == '.')
        len--;
    const char *d;
    size_t dl;
    if (au_psl_registrable(host, &d, &dl)) {
        dom = d;
        len = dl;
    }
    for (size_t i = 0; i < len; i++) {
        char ch = dom[i];
        au_buf_putc(out, ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch);
    }
}

static CookieRuleEntry *find_rule(const AuCookieJar *j, const char *key)
{
    for (size_t i = 0; i < j->nrules; i++)
        if (!strcmp(j->rules[i].site, key))
            return &j->rules[i];
    return NULL;
}

AuCookieRule au_cookie_jar_rule(const AuCookieJar *j, const char *host)
{
    if (!j->nrules || !host || !host[0])
        return AU_COOKIE_RULE_NONE;
    AuBuf k;
    au_buf_init(&k);
    au_cookie_site_key(host, &k);
    CookieRuleEntry *e = find_rule(j, au_buf_cstr(&k));
    au_buf_free(&k);
    return e ? e->rule : AU_COOKIE_RULE_NONE;
}

void au_cookie_jar_set_rule(AuCookieJar *j, const char *site, AuCookieRule r)
{
    AuBuf k;
    au_buf_init(&k);
    au_cookie_site_key(site, &k);
    const char *key = au_buf_cstr(&k);
    if (!key[0]) {
        au_buf_free(&k);
        return;
    }
    CookieRuleEntry *e = find_rule(j, key);
    if (r == AU_COOKIE_RULE_NONE) {
        if (e) {
            size_t i = (size_t)(e - j->rules);
            au_free(e->site);
            memmove(j->rules + i, j->rules + i + 1, (j->nrules - i - 1) * sizeof *j->rules);
            j->nrules--;
        }
        au_buf_free(&k);
        return;
    }
    if (!e) {
        j->rules = au_realloc(j->rules, (j->nrules + 1) * sizeof *j->rules);
        e = &j->rules[j->nrules++];
        e->site = au_strndup(key, k.len);
    }
    e->rule = r;
    /* Apply to what is stored: blocked cookies go, session-only ones stop
     * being written. */
    AuBuf ck;
    au_buf_init(&ck);
    for (size_t i = 0; i < j->n;) {
        au_cookie_site_key(j->items[i].domain, &ck);
        if (!strcmp(au_buf_cstr(&ck), key)) {
            if (r == AU_COOKIE_RULE_BLOCK) {
                remove_at(j, i);
                continue;
            }
            if (r == AU_COOKIE_RULE_SESSION && j->items[i].expires) {
                j->items[i].expires = 0;
                j->dirty = true;
            }
        }
        i++;
    }
    au_buf_free(&ck);
    au_buf_free(&k);
}

static const char *const rule_names[] = {"", "allow", "session", "block"};

const char *au_cookie_rule_name(AuCookieRule r) { return (unsigned)r < 4 ? rule_names[r] : ""; }

void au_cookie_rules_parse(AuCookieJar *j, const char *text)
{
    for (size_t i = 0; i < j->nrules; i++)
        au_free(j->rules[i].site);
    j->nrules = 0;
    if (!text)
        return;
    const char *s = text;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
            s++;
        const char *b = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r')
            s++;
        const char *eq = memchr(b, '=', (size_t)(s - b));
        if (!eq || eq == b)
            continue;
        size_t vl = (size_t)(s - eq - 1);
        AuCookieRule r = AU_COOKIE_RULE_NONE;
        for (int k = 1; k < 4; k++)
            if (strlen(rule_names[k]) == vl && !strncmp(eq + 1, rule_names[k], vl))
                r = (AuCookieRule)k;
        if (r == AU_COOKIE_RULE_NONE)
            continue;
        char *site = au_strndup(b, (size_t)(eq - b));
        au_cookie_jar_set_rule(j, site, r);
        au_free(site);
    }
}

void au_cookie_rules_serialize(const AuCookieJar *j, AuBuf *out)
{
    au_buf_clear(out);
    for (size_t i = 0; i < j->nrules; i++) {
        if (i)
            au_buf_putc(out, ' ');
        au_buf_printf(out, "%s=%s", j->rules[i].site, rule_names[j->rules[i].rule]);
    }
}

size_t au_cookie_rules_count(const AuCookieJar *j) { return j->nrules; }

const char *au_cookie_rules_at(const AuCookieJar *j, size_t i, AuCookieRule *r)
{
    if (i >= j->nrules)
        return NULL;
    if (r)
        *r = j->rules[i].rule;
    return j->rules[i].site;
}

bool au_cookie_jar_dirty(const AuCookieJar *j) { return j->dirty; }
void au_cookie_jar_clear_dirty(AuCookieJar *j) { j->dirty = false; }

/* ---- Persistence ---- */

static void put_field(AuBuf *out, const char *s)
{
    for (; *s; s++) {
        if (*s == '%')
            au_buf_puts(out, "%25");
        else if (*s == '\t')
            au_buf_puts(out, "%09");
        else if (*s == '\r')
            au_buf_puts(out, "%0D");
        else if (*s == '\n')
            au_buf_puts(out, "%0A");
        else
            au_buf_putc(out, *s);
    }
}

static const char *const ss_names[] = {"default", "none", "lax", "strict"};

void au_cookie_jar_serialize(const AuCookieJar *j, AuBuf *out, int64_t now)
{
    au_buf_puts(out, "# Aura cookies 1\n");
    for (size_t i = 0; i < j->n; i++) {
        const AuCookie *c = &j->items[i];
        if (!c->expires || c->expires <= now)
            continue;
        put_field(out, c->domain);
        au_buf_printf(out, "\t%d\t", c->host_only);
        put_field(out, c->path);
        au_buf_printf(out, "\t%d\t%d\t%s\t%d\t", c->secure, c->http_only, ss_names[c->same_site], c->partitioned);
        put_field(out, c->partition);
        /* au_put_i64, not %lld: newlib on Mac OS 9 has no %lld. */
        au_buf_putc(out, '\t');
        au_put_i64(out, c->expires);
        au_buf_putc(out, '\t');
        au_put_i64(out, c->creation);
        au_buf_putc(out, '\t');
        au_put_i64(out, c->last_access);
        au_buf_putc(out, '\t');
        put_field(out, c->name);
        au_buf_putc(out, '\t');
        put_field(out, c->value);
        au_buf_putc(out, '\n');
    }
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static char *get_field(const char *s, size_t n)
{
    AuBuf b;
    au_buf_init(&b);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n && hexv((unsigned char)s[i + 1]) >= 0 && hexv((unsigned char)s[i + 2]) >= 0) {
            au_buf_putc(&b, (char)(hexv((unsigned char)s[i + 1]) * 16 + hexv((unsigned char)s[i + 2])));
            i += 2;
        } else {
            au_buf_putc(&b, s[i]);
        }
    }
    au_buf_cstr(&b);
    return au_buf_take(&b, NULL);
}

static bool get_int(const char *s, size_t n, int64_t *v)
{
    if (!n || n > 18)
        return false;
    int64_t x = 0;
    for (size_t i = 0; i < n; i++) {
        if (!au_is_digit((unsigned char)s[i]))
            return false;
        x = x * 10 + (s[i] - '0');
    }
    *v = x;
    return true;
}

bool au_cookie_jar_parse(AuCookieJar *j, const char *data, size_t len, int64_t now)
{
    bool ok = true;
    size_t i = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && data[e] != '\n')
            e++;
        if (e > i && data[i] != '#') {
            const char *f[13];
            size_t fl[13];
            int nf = 0;
            size_t s = i;
            for (size_t k = i; k <= e && nf < 13; k++) {
                if (k == e || data[k] == '\t') {
                    f[nf] = data + s;
                    fl[nf] = k - s;
                    nf++;
                    s = k + 1;
                }
            }
            int64_t exp, cre, acc, ho, sec, ho2, part;
            int ss = -1;
            if (nf == 13)
                for (int k = 0; k < 4; k++)
                    if (au_ieq(f[5], fl[5], ss_names[k], strlen(ss_names[k])))
                        ss = k;
            bool good = nf == 13 && get_int(f[1], fl[1], &ho) && ho <= 1 && get_int(f[3], fl[3], &sec) && sec <= 1 &&
                        get_int(f[4], fl[4], &ho2) && ho2 <= 1 && ss >= 0 && get_int(f[6], fl[6], &part) &&
                        part <= 1 && get_int(f[8], fl[8], &exp) && get_int(f[9], fl[9], &cre) &&
                        get_int(f[10], fl[10], &acc) && fl[0] > 0 && fl[0] < 256 && fl[2] > 0 && fl[2] < MAX_ATTR &&
                        fl[11] + fl[12] <= 3 * MAX_NAME_VALUE && fl[7] < 512;
            if (good && exp > now && j->n < AU_COOKIES_TOTAL) {
                AuCookie c;
                memset(&c, 0, sizeof c);
                c.domain = get_field(f[0], fl[0]);
                c.host_only = ho;
                c.path = get_field(f[2], fl[2]);
                c.secure = sec;
                c.http_only = ho2;
                c.same_site = (AuSameSite)ss;
                c.partitioned = part;
                c.partition = get_field(f[7], fl[7]);
                c.expires = exp > now + AU_COOKIE_MAX_AGE_SECONDS ? now + AU_COOKIE_MAX_AGE_SECONDS : exp;
                c.creation = cre;
                c.last_access = acc;
                c.name = get_field(f[11], fl[11]);
                c.value = get_field(f[12], fl[12]);
                if (c.path[0] != '/' || find_same(j, &c) || strlen(c.name) + strlen(c.value) > MAX_NAME_VALUE ||
                    ctl_in(c.name, strlen(c.name)) || ctl_in(c.value, strlen(c.value))) {
                    cookie_free(&c);
                    ok = false;
                } else {
                    if (j->n == j->cap) {
                        j->cap = j->cap ? j->cap * 2 : 64;
                        j->items = au_realloc(j->items, j->cap * sizeof *j->items);
                    }
                    j->items[j->n++] = c;
                }
            } else if (!good) {
                ok = false;
            }
        }
        i = e + 1;
    }
    j->dirty = false;
    return ok;
}
