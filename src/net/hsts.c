/* HSTS store, header parsing and preload list. */
#include "hsts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct AuHsts {
    AuHstsEntry *items;
    size_t n, cap;
    bool dirty;
};

/* The built-in preload list: sorted, each with includeSubDomains as in
 * Chromium's list for these domains. */
static const struct {
    const char *host;
    bool sub;
} preload[] = {
    {"duckduckgo.com", true},
    {"github.com", true},
    {"news.ycombinator.com", true},
    {"reddit.com", true},
    {"wikipedia.org", true},
    {"ycombinator.com", false},
};

AuHsts *au_hsts_new(void)
{
    return au_calloc(sizeof(AuHsts));
}

void au_hsts_free(AuHsts *h)
{
    if (!h)
        return;
    for (size_t i = 0; i < h->n; i++)
        au_free(h->items[i].host);
    au_free(h->items);
    au_free(h);
}

static bool is_ip(const char *host)
{
    size_t n = strlen(host);
    return host[0] == '[' || (n && au_is_digit((unsigned char)host[n - 1]));
}

/* host equals d, or ends with "." d */
static bool sub_of(const char *host, const char *d, bool *exact)
{
    size_t hn = strlen(host), dn = strlen(d);
    *exact = hn == dn && !memcmp(host, d, dn);
    return *exact || (hn > dn && host[hn - dn - 1] == '.' && !memcmp(host + hn - dn, d, dn));
}

bool au_hsts_preloaded(const char *host)
{
    for (size_t i = 0; i < sizeof preload / sizeof preload[0]; i++) {
        bool exact;
        if (sub_of(host, preload[i].host, &exact) && (exact || preload[i].sub))
            return true;
    }
    return false;
}

static void remove_at(AuHsts *h, size_t i)
{
    au_free(h->items[i].host);
    memmove(h->items + i, h->items + i + 1, (h->n - i - 1) * sizeof *h->items);
    h->n--;
}

bool au_hsts_is_known(AuHsts *h, const char *host, int64_t now)
{
    if (!host[0] || is_ip(host))
        return false;
    if (au_hsts_preloaded(host))
        return true;
    if (!h)
        return false;
    for (size_t i = 0; i < h->n;) {
        AuHstsEntry *e = &h->items[i];
        if (e->expires <= now) {
            remove_at(h, i);
            h->dirty = true;
            continue;
        }
        bool exact;
        if (sub_of(host, e->host, &exact) && (exact || e->include_subdomains))
            return true;
        i++;
    }
    return false;
}

bool au_hsts_parse(const char *v, int64_t *max_age, bool *sub)
{
    /* directives separated by ';': max-age (required, once),
     * includeSubDomains (once); unknown directives ignored; duplicates
     * make the header invalid. */
    bool seen_age = false, seen_sub = false;
    *sub = false;
    *max_age = 0;
    const char *s = v;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == ';')
            s++;
        if (!*s)
            break;
        const char *n = s;
        while (*s && *s != '=' && *s != ';' && *s != ' ' && *s != '\t')
            s++;
        size_t nl = (size_t)(s - n);
        while (*s == ' ' || *s == '\t')
            s++;
        char val[32];
        size_t vl = 0;
        bool has_val = false;
        if (*s == '=') {
            has_val = true;
            s++;
            while (*s == ' ' || *s == '\t')
                s++;
            bool q = *s == '"';
            if (q)
                s++;
            while (*s && (q ? *s != '"' : (*s != ';' && *s != ' ' && *s != '\t'))) {
                if (vl + 1 < sizeof val)
                    val[vl++] = *s;
                else
                    return false;
                s++;
            }
            if (q && *s == '"')
                s++;
        }
        val[vl] = 0;
        while (*s && *s != ';')
            s++; /* junk after a directive */
        if (au_ieq(n, nl, "max-age", 7)) {
            if (seen_age || !has_val || !vl)
                return false;
            int64_t a = 0;
            for (size_t i = 0; i < vl; i++) {
                if (!au_is_digit((unsigned char)val[i]))
                    return false;
                if (a < 100000000000LL)
                    a = a * 10 + (val[i] - '0');
            }
            *max_age = a;
            seen_age = true;
        } else if (au_ieq(n, nl, "includeSubDomains", 17)) {
            if (seen_sub || has_val)
                return false;
            *sub = seen_sub = true;
        }
    }
    return seen_age;
}

bool au_hsts_process(AuHsts *h, const char *host, const char *value, int64_t now)
{
    int64_t age;
    bool sub;
    if (!au_hsts_parse(value, &age, &sub) || is_ip(host) || !host[0])
        return false;
    for (size_t i = 0; i < h->n; i++)
        if (!strcmp(h->items[i].host, host)) {
            if (age == 0)
                remove_at(h, i);
            else {
                h->items[i].expires = now + age;
                h->items[i].include_subdomains = sub;
            }
            h->dirty = true;
            return true;
        }
    if (age == 0)
        return true;
    if (h->n >= AU_HSTS_MAX_ENTRIES) {
        /* Drop the entry that expires first. */
        size_t victim = 0;
        for (size_t i = 1; i < h->n; i++)
            if (h->items[i].expires < h->items[victim].expires)
                victim = i;
        remove_at(h, victim);
    }
    if (h->n == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 32;
        h->items = au_realloc(h->items, h->cap * sizeof *h->items);
    }
    h->items[h->n].host = au_strndup(host, strlen(host));
    h->items[h->n].expires = now + age;
    h->items[h->n].include_subdomains = sub;
    h->n++;
    h->dirty = true;
    return true;
}

void au_hsts_remove(AuHsts *h, const char *host)
{
    for (size_t i = 0; i < h->n; i++)
        if (!strcmp(h->items[i].host, host)) {
            remove_at(h, i);
            h->dirty = true;
            return;
        }
}

size_t au_hsts_count(const AuHsts *h) { return h->n; }
const AuHstsEntry *au_hsts_at(const AuHsts *h, size_t i) { return i < h->n ? &h->items[i] : NULL; }
bool au_hsts_dirty(const AuHsts *h) { return h->dirty; }
void au_hsts_clear_dirty(AuHsts *h) { h->dirty = false; }

void au_hsts_serialize(const AuHsts *h, AuBuf *out, int64_t now)
{
    au_buf_puts(out, "# Aura HSTS 1\n");
    for (size_t i = 0; i < h->n; i++)
        if (h->items[i].expires > now)
            au_buf_printf(out, "%s\t%lld\t%d\n", h->items[i].host, (long long)h->items[i].expires,
                          h->items[i].include_subdomains ? 1 : 0);
}

bool au_hsts_parse_file(AuHsts *h, const char *data, size_t len, int64_t now)
{
    bool ok = true;
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && data[j] != '\n')
            j++;
        if (j > i && data[i] != '#') {
            /* host \t expires \t sub, host a lower case domain */
            const char *l = data + i;
            size_t ll = j - i;
            const char *t1 = memchr(l, '\t', ll);
            const char *t2 = t1 ? memchr(t1 + 1, '\t', ll - (size_t)(t1 + 1 - l)) : NULL;
            bool good = t1 && t2 && t1 > l && t1 - l < 254 && (size_t)(l + ll - t2) == 2 &&
                        (t2[1] == '0' || t2[1] == '1');
            int64_t exp = 0;
            if (good) {
                for (const char *p = l; p < t1; p++)
                    if (!(au_is_alpha((unsigned char)*p) || au_is_digit((unsigned char)*p) || *p == '.' || *p == '-'))
                        good = false;
                if (t2 - t1 < 2 || t2 - t1 > 16)
                    good = false;
                for (const char *p = t1 + 1; good && p < t2; p++) {
                    if (!au_is_digit((unsigned char)*p))
                        good = false;
                    else
                        exp = exp * 10 + (*p - '0');
                }
            }
            if (good && exp > now) {
                char host[256];
                memcpy(host, l, (size_t)(t1 - l));
                host[t1 - l] = 0;
                char val[64];
                snprintf(val, sizeof val, "max-age=%lld%s", (long long)(exp - now), t2[1] == '1' ? "; includeSubDomains" : "");
                au_hsts_process(h, host, val, now);
            } else if (!good) {
                ok = false;
            }
        }
        i = j + 1;
    }
    h->dirty = false;
    return ok;
}
