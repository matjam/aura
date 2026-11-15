/* HTTP cache tests: Cache-Control parsing, freshness and age tables,
 * storability, Vary, eviction and limits, serialization and the storage
 * interface, and the client's use of the cache (fresh hits, revalidation
 * with 304, no-store, invalidation by POST, reload). */
#include "fakenet.h"
#include "net/cache.h"
#include "net/disk_cache.h"
#include "net/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...)                                                                                          \
    do {                                                                                                          \
        checks++;                                                                                                 \
        if (!(cond)) {                                                                                            \
            failures++;                                                                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                           \
            printf(__VA_ARGS__);                                                                                  \
            printf("\n");                                                                                         \
        }                                                                                                         \
    } while (0)

#define T0 1800000000LL
/* "Fri, 15 Jan 2027 08:00:00 GMT" is T0. */

static void hdrs(AuHeaders *h, const char *spec)
{
    /* "Name: value\n..." */
    au_headers_init(h);
    while (*spec) {
        const char *colon = strchr(spec, ':'), *nl = strchr(spec, '\n');
        if (!nl)
            nl = spec + strlen(spec);
        const char *v = colon + 1;
        while (*v == ' ')
            v++;
        au_headers_addn(h, spec, (size_t)(colon - spec), v, (size_t)(nl - v));
        spec = *nl ? nl + 1 : nl;
    }
}

static void date_str(int64_t t, char out[32]) { au_http_format_date(t, out); }

static void test_control(void)
{
    AuHeaders h;
    AuCacheControl cc;
    hdrs(&h, "Cache-Control: public, max-age=60, s-maxage=\"30\"\nCache-Control: no-cache=\"set-cookie\", immutable");
    au_cache_control_parse(&h, &cc);
    CHECK(cc.public_ && cc.max_age == 60 && cc.s_maxage == 30 && cc.no_cache && cc.immutable, "parse");
    au_headers_free(&h);
    hdrs(&h, "Cache-Control: max-age=60, max-age=10, MUST-REVALIDATE, no-store, private, max-stale, min-fresh=5");
    au_cache_control_parse(&h, &cc);
    CHECK(cc.max_age == 10 && cc.must_revalidate && cc.no_store && cc.private_ && cc.max_stale == INT64_MAX &&
              cc.min_fresh == 5,
          "duplicates take the lower max-age; flags");
    au_headers_free(&h);
    hdrs(&h, "Cache-Control: max-age=abc, max-age=-1");
    au_cache_control_parse(&h, &cc);
    CHECK(cc.max_age == -1, "invalid max-age ignored: %lld", (long long)cc.max_age);
    au_headers_free(&h);
}

static void test_freshness(void)
{
    char d[32], e[32], lm[32];
    date_str(T0, d);
    date_str(T0 + 3600, e);
    date_str(T0 - 100 * 3600, lm);
    char spec[256];
    AuHeaders h;

    snprintf(spec, sizeof spec, "Date: %s\nCache-Control: max-age=120\nExpires: %s", d, e);
    hdrs(&h, spec);
    CHECK(au_cache_freshness_lifetime(200, &h) == 120, "max-age wins over Expires");
    au_headers_free(&h);

    snprintf(spec, sizeof spec, "Date: %s\nExpires: %s", d, e);
    hdrs(&h, spec);
    CHECK(au_cache_freshness_lifetime(200, &h) == 3600, "Expires - Date");
    au_headers_free(&h);

    snprintf(spec, sizeof spec, "Date: %s\nExpires: 0", d);
    hdrs(&h, spec);
    CHECK(au_cache_freshness_lifetime(200, &h) == 0, "invalid Expires: expired");
    au_headers_free(&h);

    snprintf(spec, sizeof spec, "Date: %s\nLast-Modified: %s", d, lm);
    hdrs(&h, spec);
    CHECK(au_cache_freshness_lifetime(200, &h) == 36000, "heuristic 10%%: %lld",
          (long long)au_cache_freshness_lifetime(200, &h));
    CHECK(au_cache_freshness_lifetime(302, &h) == 0, "no heuristic for 302");
    au_headers_free(&h);

    date_str(T0 - 400 * 86400LL, lm);
    snprintf(spec, sizeof spec, "Date: %s\nLast-Modified: %s", d, lm);
    hdrs(&h, spec);
    CHECK(au_cache_freshness_lifetime(200, &h) == 86400, "heuristic capped at a day");
    au_headers_free(&h);

    hdrs(&h, "Content-Type: text/html");
    CHECK(au_cache_freshness_lifetime(200, &h) == 0, "nothing: 0");
    au_headers_free(&h);

    /* Age (RFC 9111 4.2.3). */
    snprintf(spec, sizeof spec, "Date: %s\nAge: 30", d);
    hdrs(&h, spec);
    CHECK(au_cache_current_age(&h, T0, T0 + 2, T0 + 2) == 32, "age + response delay: %lld",
          (long long)au_cache_current_age(&h, T0, T0 + 2, T0 + 2));
    CHECK(au_cache_current_age(&h, T0, T0 + 2, T0 + 102) == 132, "plus resident time");
    au_headers_free(&h);
    date_str(T0 - 50, d);
    snprintf(spec, sizeof spec, "Date: %s", d);
    hdrs(&h, spec);
    CHECK(au_cache_current_age(&h, T0, T0, T0) == 50, "apparent age from Date");
    au_headers_free(&h);
}

static AuCacheWriter *store(AuHttpCache *c, const char *url, const char *req_spec, int status, const char *resp_spec,
                            const char *body, int64_t t)
{
    AuHeaders req, resp;
    hdrs(&req, req_spec);
    hdrs(&resp, resp_spec);
    AuCacheWriter *w = au_cache_begin(c, url, &req, status, "OK", &resp, t, t);
    if (w && body && !au_cache_write(w, body, strlen(body)))
        w = NULL;
    if (w)
        au_cache_commit(w);
    au_headers_free(&req);
    au_headers_free(&resp);
    return w;
}

static AuCacheState look(AuHttpCache *c, const char *url, const char *req_spec, int64_t now, char *body, size_t cap)
{
    AuHeaders req;
    hdrs(&req, req_spec);
    AuCacheEntry *e = NULL;
    AuCacheState st = au_cache_lookup(c, url, &req, now, &e);
    if (e) {
        size_t n;
        const uint8_t *b = au_cache_entry_body(e, &n);
        if (body) {
            snprintf(body, cap, "%.*s", (int)n, (const char *)b);
        }
        au_cache_entry_release(c, e);
    }
    au_headers_free(&req);
    return st;
}

static void test_store_lookup(void)
{
    AuHttpCache *c = au_cache_new(1 << 20);
    char d[32], body[64];
    date_str(T0, d);
    char spec[256];
    snprintf(spec, sizeof spec, "Date: %s\nCache-Control: max-age=100\nContent-Type: text/plain\nSet-Cookie: s=1\n"
                                "Content-Encoding: gzip",
             d);
    CHECK(store(c, "http://a.test/x", "Accept: */*", 200, spec, "hello", T0) != NULL, "stored");
    CHECK(look(c, "http://a.test/x", "Accept: */*", T0 + 50, body, sizeof body) == AU_CACHE_FRESH &&
              !strcmp(body, "hello"),
          "fresh hit");
    CHECK(look(c, "http://a.test/x", "Accept: */*", T0 + 150, NULL, 0) == AU_CACHE_MISS,
          "stale without validator: miss");
    CHECK(look(c, "http://a.test/y", "", T0, NULL, 0) == AU_CACHE_MISS, "other URL");
    CHECK(look(c, "http://a.test/x", "Cache-Control: no-cache", T0 + 1, NULL, 0) == AU_CACHE_MISS,
          "request no-cache without validator");
    CHECK(look(c, "http://a.test/x", "Cache-Control: max-age=10", T0 + 20, NULL, 0) == AU_CACHE_MISS,
          "request max-age");
    CHECK(look(c, "http://a.test/x", "Cache-Control: max-stale=100", T0 + 150, NULL, 0) == AU_CACHE_FRESH,
          "max-stale accepts stale");
    CHECK(look(c, "http://a.test/x", "Cache-Control: no-store", T0, NULL, 0) == AU_CACHE_MISS, "request no-store");
    /* Dropped fields. */
    AuHeaders req;
    au_headers_init(&req);
    AuCacheEntry *e;
    au_cache_lookup(c, "http://a.test/x", &req, T0, &e);
    CHECK(e && !au_headers_get(au_cache_entry_headers(e), "set-cookie") &&
              !au_headers_get(au_cache_entry_headers(e), "content-encoding") &&
              au_headers_get(au_cache_entry_headers(e), "content-type"),
          "Set-Cookie and Content-Encoding not kept");
    au_cache_entry_release(c, e);

    /* Validators: stale entries come back STALE. */
    snprintf(spec, sizeof spec, "Date: %s\nCache-Control: max-age=10\nETag: \"v1\"", d);
    store(c, "http://a.test/e", "", 200, spec, "etag body", T0);
    CHECK(look(c, "http://a.test/e", "", T0 + 5, NULL, 0) == AU_CACHE_FRESH, "fresh with etag");
    CHECK(look(c, "http://a.test/e", "", T0 + 50, NULL, 0) == AU_CACHE_STALE, "stale with etag");
    snprintf(spec, sizeof spec, "Date: %s\nCache-Control: no-cache\nLast-Modified: %s", d, d);
    store(c, "http://a.test/nc", "", 200, spec, "x", T0);
    CHECK(look(c, "http://a.test/nc", "", T0, NULL, 0) == AU_CACHE_STALE, "no-cache always revalidates");
    snprintf(spec, sizeof spec, "Date: %s\nCache-Control: max-age=10, must-revalidate\nETag: \"m\"", d);
    store(c, "http://a.test/mr", "", 200, spec, "x", T0);
    CHECK(look(c, "http://a.test/mr", "Cache-Control: max-stale", T0 + 50, NULL, 0) == AU_CACHE_STALE,
          "must-revalidate ignores max-stale");

    /* 304 update. */
    au_cache_lookup(c, "http://a.test/e", &req, T0 + 50, &e);
    AuHeaders r304;
    char d2[32];
    date_str(T0 + 50, d2);
    snprintf(spec, sizeof spec, "Date: %s\nCache-Control: max-age=100\nETag: \"v1\"\nContent-Length: 0", d2);
    hdrs(&r304, spec);
    au_cache_update(c, e, &r304, T0 + 50, T0 + 50);
    au_cache_entry_release(c, e);
    au_headers_free(&r304);
    CHECK(look(c, "http://a.test/e", "", T0 + 120, body, sizeof body) == AU_CACHE_FRESH && !strcmp(body, "etag body"),
          "304 refreshed the entry");

    /* Not storable. */
    snprintf(spec, sizeof spec, "Date: %s\nCache-Control: no-store, max-age=100", d);
    CHECK(!store(c, "http://a.test/ns", "", 200, spec, "x", T0), "no-store");
    snprintf(spec, sizeof spec, "Date: %s\nCache-Control: max-age=100", d);
    CHECK(!store(c, "http://a.test/r", "Cache-Control: no-store", 200, spec, "x", T0), "request no-store");
    CHECK(!store(c, "http://a.test/a", "Authorization: Basic eDp5", 200, spec, "x", T0), "authorized");
    snprintf(spec, sizeof spec, "Date: %s\nCache-Control: max-age=100, public", d);
    CHECK(store(c, "http://a.test/ap", "Authorization: Basic eDp5", 200, spec, "x", T0) != NULL,
          "authorized but public");
    snprintf(spec, sizeof spec, "Date: %s\nCache-Control: max-age=100", d);
    CHECK(!store(c, "http://a.test/p", "", 206, spec, "x", T0), "206");
    CHECK(!store(c, "http://a.test/p", "", 302, spec, "x", T0), "302");
    CHECK(!store(c, "http://a.test/p", "", 500, spec, "x", T0), "500");
    CHECK(store(c, "http://a.test/404", "", 404, spec, "x", T0) != NULL, "404 with max-age");
    snprintf(spec, sizeof spec, "Date: %s", d);
    CHECK(store(c, "http://a.test/none", "", 200, spec, "x", T0) != NULL, "neither freshness nor validator: kept");
    CHECK(look(c, "http://a.test/none", "", T0, NULL, 0) == AU_CACHE_MISS, "neither freshness nor validator: miss");
    {
        AuHeaders hq;
        au_headers_init(&hq);
        AuCacheEntry *he = NULL;
        CHECK(au_cache_lookup_history(c, "http://a.test/none", &hq, &he) == AU_CACHE_FRESH,
              "neither freshness nor validator: history hit");
        if (he)
            au_cache_entry_release(c, he);
        au_headers_free(&hq);
    }
    snprintf(spec, sizeof spec, "Date: %s\nCache-Control: max-age=100\nVary: *", d);
    CHECK(!store(c, "http://a.test/vs", "", 200, spec, "x", T0), "Vary: *");

    /* Vary. */
    snprintf(spec, sizeof spec, "Date: %s\nCache-Control: max-age=100\nVary: Accept-Language, X-Absent", d);
    store(c, "http://a.test/v", "Accept-Language: en", 200, spec, "english", T0);
    store(c, "http://a.test/v", "Accept-Language: fr", 200, spec, "french", T0);
    CHECK(look(c, "http://a.test/v", "Accept-Language: fr", T0, body, sizeof body) == AU_CACHE_FRESH &&
              !strcmp(body, "french"),
          "vary fr: %s", body);
    CHECK(look(c, "http://a.test/v", "Accept-Language: en", T0, body, sizeof body) == AU_CACHE_FRESH &&
              !strcmp(body, "english"),
          "vary en: %s", body);
    CHECK(look(c, "http://a.test/v", "Accept-Language: de", T0, NULL, 0) == AU_CACHE_MISS, "vary de: miss");
    CHECK(look(c, "http://a.test/v", "Accept-Language: en\nX-Absent: 1", T0, NULL, 0) == AU_CACHE_MISS,
          "vary: a field absent then must stay absent");

    /* History lookups honour Vary (a page that varies on Cookie is not
     * shown to another cookie) and the request's no-store. */
    snprintf(spec, sizeof spec, "Date: %s\nCache-Control: no-cache\nETag: \"c\"\nVary: Cookie", d);
    store(c, "http://a.test/vc", "Cookie: id=alice", 200, spec, "alice", T0);
    {
        AuHeaders hq;
        hdrs(&hq, "Cookie: id=alice");
        CHECK(au_cache_lookup_history(c, "http://a.test/vc", &hq, &e) == AU_CACHE_FRESH && e, "history hit");
        if (e)
            au_cache_entry_release(c, e);
        au_headers_free(&hq);
        hdrs(&hq, "Cookie: id=bob");
        CHECK(au_cache_lookup_history(c, "http://a.test/vc", &hq, &e) == AU_CACHE_MISS && !e,
              "history: other cookie misses");
        au_headers_free(&hq);
        hdrs(&hq, "Cookie: id=alice\nCache-Control: no-store");
        CHECK(au_cache_lookup_history(c, "http://a.test/vc", &hq, &e) == AU_CACHE_MISS, "history: request no-store");
        au_headers_free(&hq);
    }

    /* Invalidation removes every variant. */
    au_cache_invalidate(c, "http://a.test/v");
    CHECK(look(c, "http://a.test/v", "Accept-Language: en", T0, NULL, 0) == AU_CACHE_MISS, "invalidated");
    au_headers_free(&req);
    au_cache_free(c);
}

static void test_limits(void)
{
    /* Budget 32 KB: entry limit 8 KB. */
    AuHttpCache *c = au_cache_new(32 * 1024);
    CHECK(au_cache_entry_limit(c) == 8192, "entry limit %zu", au_cache_entry_limit(c));
    char d[32], spec[128], url[64];
    date_str(T0, d);
    snprintf(spec, sizeof spec, "Date: %s\nCache-Control: max-age=1000", d);
    char *big = malloc(9000);
    memset(big, 'b', 8999);
    big[8999] = 0;
    CHECK(!store(c, "http://l.test/big", "", 200, spec, big, T0), "over the entry limit");
    big[6000] = 0;
    for (int i = 0; i < 20; i++) {
        snprintf(url, sizeof url, "http://l.test/%d", i);
        store(c, url, "", 200, spec, big, T0);
        if (i == 1) /* keep entry 0 recently used */
            look(c, "http://l.test/0", "", T0, NULL, 0);
    }
    CHECK(au_cache_bytes(c) <= 32 * 1024, "within budget: %zu", au_cache_bytes(c));
    CHECK(au_cache_count(c) < 20 && au_cache_count(c) >= 3, "evicted some: %zu", au_cache_count(c));
    CHECK(look(c, "http://l.test/19", "", T0, NULL, 0) == AU_CACHE_FRESH, "newest kept");
    CHECK(look(c, "http://l.test/2", "", T0, NULL, 0) == AU_CACHE_MISS, "least recently used evicted");
    /* An entry in use survives eviction. */
    AuHeaders req;
    au_headers_init(&req);
    AuCacheEntry *e;
    CHECK(au_cache_lookup(c, "http://l.test/19", &req, T0, &e) == AU_CACHE_FRESH, "pin");
    au_cache_set_max(c, 0);
    CHECK(au_cache_count(c) == 0, "emptied");
    size_t n;
    CHECK(au_cache_entry_body(e, &n) && n == 6000, "pinned entry still readable");
    au_cache_entry_release(c, e);
    CHECK(!store(c, "http://l.test/z", "", 200, spec, "x", T0), "budget 0 stores nothing");
    au_headers_free(&req);
    free(big);
    au_cache_free(c);
}

/* An in-memory AuCacheStorage. */
typedef struct Disk {
    char key[8][128];
    AuBuf val[8];
    int n;
    int loads, stores;
} Disk;

static void d_store(void *ctx, const char *key, const void *data, size_t len)
{
    Disk *d = ctx;
    int i;
    for (i = 0; i < d->n && strcmp(d->key[i], key); i++) {
    }
    if (i == d->n) {
        if (d->n == 8)
            return;
        snprintf(d->key[d->n], sizeof d->key[0], "%s", key);
        au_buf_init(&d->val[d->n]);
        d->n++;
    }
    au_buf_clear(&d->val[i]);
    au_buf_append(&d->val[i], data, len);
    d->stores++;
}

static bool d_load(void *ctx, const char *key, AuBuf *out)
{
    Disk *d = ctx;
    d->loads++;
    for (int i = 0; i < d->n; i++)
        if (!strcmp(d->key[i], key)) {
            au_buf_append(out, d->val[i].data, d->val[i].len);
            return true;
        }
    return false;
}

static void d_remove(void *ctx, const char *key)
{
    Disk *d = ctx;
    for (int i = 0; i < d->n; i++)
        if (!strcmp(d->key[i], key))
            d->key[i][0] = 0;
}

static void test_storage(void)
{
    Disk disk;
    memset(&disk, 0, sizeof disk);
    AuCacheStorage st = {&disk, d_store, d_load, d_remove};
    AuHttpCache *c = au_cache_new(1 << 20);
    au_cache_set_storage(c, &st);
    char d[32], spec[160], body[64];
    date_str(T0, d);
    snprintf(spec, sizeof spec, "Date: %s\nCache-Control: max-age=100\nETag: \"x\"\nVary: Accept", d);
    store(c, "http://s.test/", "Accept: text/html", 200, spec, "disk body", T0);
    CHECK(disk.stores == 1, "written to storage");
    /* A second cache (a new session) finds it through storage. */
    AuHttpCache *c2 = au_cache_new(1 << 20);
    au_cache_set_storage(c2, &st);
    CHECK(look(c2, "http://s.test/", "Accept: text/html", T0 + 10, body, sizeof body) == AU_CACHE_FRESH &&
              !strcmp(body, "disk body"),
          "loaded from storage: %s", body);
    CHECK(look(c2, "http://s.test/", "Accept: image/png", T0 + 10, NULL, 0) == AU_CACHE_MISS, "vary kept");
    /* Serialization round trip and damaged data. */
    AuHeaders req;
    au_headers_init(&req);
    au_headers_add(&req, "Accept", "text/html");
    AuCacheEntry *e;
    au_cache_lookup(c2, "http://s.test/", &req, T0, &e);
    AuBuf b;
    au_buf_init(&b);
    au_cache_entry_serialize(e, &b);
    au_cache_entry_release(c2, e);
    AuCacheEntry *p = au_cache_entry_parse(b.data, b.len);
    size_t n;
    CHECK(p && au_cache_entry_status(p) == 200 && au_cache_entry_body(p, &n) && n == 9 &&
              !strcmp(au_cache_entry_etag(p), "\"x\""),
          "round trip");
    au_cache_entry_free(p);
    int bad = 0;
    for (size_t cut = 0; cut < b.len; cut++) {
        AuCacheEntry *q = au_cache_entry_parse(b.data, cut);
        if (q) {
            bad++;
            au_cache_entry_free(q);
        }
    }
    CHECK(bad == 0, "every truncation refused (%d accepted)", bad);
    b.data[10] ^= 0x7f; /* the high byte of the reason's length */
    CHECK(!au_cache_entry_parse(b.data, b.len), "bad length refused");
    au_buf_free(&b);
    au_headers_free(&req);
    au_cache_free(c);
    au_cache_free(c2);
    for (int i = 0; i < disk.n; i++)
        au_buf_free(&disk.val[i]);
}

/* ---- The disk cache over an in-memory folder ---- */
typedef struct MemDir {
    char name[16][24];
    AuBuf data[16];
    uint32_t mtime[16];
    bool used[16];
    int writes, removes;
    bool fail_writes;
} MemDir;

static int md_find(MemDir *m, const char *name)
{
    for (int i = 0; i < 16; i++)
        if (m->used[i] && !strcmp(m->name[i], name))
            return i;
    return -1;
}

static void md_list(void *ctx, void (*each)(void *, const char *, size_t, uint32_t), void *ec)
{
    MemDir *m = ctx;
    for (int i = 0; i < 16; i++)
        if (m->used[i])
            each(ec, m->name[i], m->data[i].len, m->mtime[i]);
}

static bool md_read(void *ctx, const char *name, AuBuf *out)
{
    MemDir *m = ctx;
    int i = md_find(m, name);
    if (i < 0)
        return false;
    au_buf_append(out, m->data[i].data, m->data[i].len);
    return true;
}

static bool md_write(void *ctx, const char *name, const void *data, size_t len)
{
    MemDir *m = ctx;
    if (m->fail_writes)
        return false;
    int i = md_find(m, name);
    if (i < 0) {
        for (i = 0; i < 16 && m->used[i]; i++) {
        }
        if (i == 16)
            return false;
        m->used[i] = true;
        snprintf(m->name[i], sizeof m->name[i], "%s", name);
        au_buf_init(&m->data[i]);
    }
    au_buf_clear(&m->data[i]);
    au_buf_append(&m->data[i], data, len);
    m->writes++;
    return true;
}

static void md_remove(void *ctx, const char *name)
{
    MemDir *m = ctx;
    int i = md_find(m, name);
    if (i >= 0) {
        au_buf_free(&m->data[i]);
        m->used[i] = false;
        m->removes++;
    }
}

static void test_disk_cache(void)
{
    MemDir m;
    memset(&m, 0, sizeof m);
    AuDiskCacheFiles files = {&m, md_list, md_read, md_write, md_remove};
    AuDiskCache *dc = au_disk_cache_new(&files, 4000);
    AuCacheStorage st;
    au_disk_cache_storage(dc, &st);
    char n1[19], n2[19];
    au_disk_cache_name("http://a.test/", n1);
    au_disk_cache_name("http://b.test/", n2);
    CHECK(strlen(n1) == 17 && n1[0] == 'c' && strcmp(n1, n2), "names: %s %s", n1, n2);
    char big[600];
    memset(big, 'x', sizeof big);
    st.store(st.ctx, "http://a.test/", big, 500);
    st.store(st.ctx, "http://b.test/", "bee", 3);
    CHECK(au_disk_cache_count(dc) == 2 && m.writes == 2, "two files");
    AuBuf b;
    au_buf_init(&b);
    CHECK(st.load(st.ctx, "http://b.test/", &b) && b.len == 3 && !memcmp(b.data, "bee", 3), "load");
    CHECK(!st.load(st.ctx, "http://c.test/", &b), "miss");
    /* Too large for a quarter of the budget: not written, old copy gone. */
    st.store(st.ctx, "http://b.test/", big, 1001);
    CHECK(au_disk_cache_count(dc) == 1 && md_find(&m, n2) < 0, "oversized entry dropped");
    /* Least recently used goes first. */
    for (int i = 0; i < 8; i++) {
        char key[32];
        snprintf(key, sizeof key, "http://k%d.test/", i);
        st.store(st.ctx, key, big, 500);
        if (i == 3)
            CHECK(st.load(st.ctx, "http://a.test/", &b), "touch a");
    }
    CHECK(au_disk_cache_bytes(dc) <= 4000, "within budget: %zu", au_disk_cache_bytes(dc));
    CHECK(st.load(st.ctx, "http://a.test/", &b), "recently used a kept");
    CHECK(!st.load(st.ctx, "http://k0.test/", &b), "oldest evicted");
    /* A damaged file is a miss and is removed. */
    int ia = md_find(&m, n1);
    m.data[ia].data[2] = 'X';
    CHECK(!st.load(st.ctx, "http://a.test/", &b) && md_find(&m, n1) < 0, "damaged file removed");
    /* A new session indexes the folder by mtime, ignoring strangers. */
    md_write(&m, "Cache.tmp", "zz", 2);
    for (int i = 0; i < 16; i++)
        m.mtime[i] = (uint32_t)i;
    AuDiskCache *dc2 = au_disk_cache_new(&files, 4000);
    AuCacheStorage st2;
    au_disk_cache_storage(dc2, &st2);
    size_t before = au_disk_cache_count(dc2);
    CHECK(before == au_disk_cache_count(dc), "listed %zu, had %zu", before, au_disk_cache_count(dc));
    CHECK(st2.load(st2.ctx, "http://k7.test/", &b), "found by the new session");
    au_disk_cache_set_max(dc2, 1200);
    CHECK(au_disk_cache_bytes(dc2) <= 1200 && st2.load(st2.ctx, "http://k7.test/", &b), "shrink keeps newest");
    st2.remove(st2.ctx, "http://k7.test/");
    CHECK(!st2.load(st2.ctx, "http://k7.test/", &b), "removed");
    au_disk_cache_clear(dc2);
    CHECK(au_disk_cache_count(dc2) == 0 && md_find(&m, "Cache.tmp") >= 0, "clear leaves strangers");
    /* With the HTTP cache in front. */
    AuHttpCache *c = au_cache_new(1 << 20);
    au_cache_set_storage(c, &st2);
    char d[32], spec[160], body[64];
    date_str(T0, d);
    snprintf(spec, sizeof spec, "Date: %s\nCache-Control: max-age=100", d);
    au_disk_cache_set_max(dc2, 1 << 20);
    store(c, "http://s.test/", "", 200, spec, "on disk", T0);
    AuHttpCache *c2 = au_cache_new(1 << 20);
    au_cache_set_storage(c2, &st2);
    CHECK(look(c2, "http://s.test/", "", T0 + 1, body, sizeof body) == AU_CACHE_FRESH && !strcmp(body, "on disk"),
          "HTTP cache through the disk cache: %s", body);
    au_cache_free(c);
    au_cache_free(c2);

    /* Deferred writes: a store only queues; lookups see the queued bytes;
     * idle writes them oldest first; flush and free write the rest. */
    MemDir q;
    memset(&q, 0, sizeof q);
    AuDiskCacheFiles qfiles = {&q, md_list, md_read, md_write, md_remove};
    AuDiskCache *dq = au_disk_cache_new(&qfiles, 16 << 20);
    AuCacheStorage sq;
    au_disk_cache_storage(dq, &sq);
    au_disk_cache_set_deferred(dq, true);
    sq.store(sq.ctx, "http://a.test/", "one", 3);
    sq.store(sq.ctx, "http://b.test/", "two", 3);
    sq.store(sq.ctx, "http://c.test/", "three", 5);
    CHECK(q.writes == 0 && au_disk_cache_pending(dq) == 3 && au_disk_cache_count(dq) == 3, "stores are queued: %d",
          q.writes);
    CHECK(sq.load(sq.ctx, "http://b.test/", &b) && b.len == 3 && !memcmp(b.data, "two", 3) && q.writes == 0,
          "a queued entry is read from the queue");
    sq.store(sq.ctx, "http://a.test/", "uno", 3);
    CHECK(au_disk_cache_pending(dq) == 3, "a newer store replaces the queued one");
    CHECK(au_disk_cache_idle(dq, 1) && q.writes == 1 && md_find(&q, n2) >= 0, "idle writes one, the oldest (b)");
    sq.remove(sq.ctx, "http://c.test/");
    CHECK(au_disk_cache_pending(dq) == 1 && !sq.load(sq.ctx, "http://c.test/", &b), "a removed entry is never written");
    au_disk_cache_flush(dq);
    CHECK(q.writes == 2 && au_disk_cache_pending(dq) == 0 && md_find(&q, n1) >= 0, "flush writes the rest");
    CHECK(sq.load(sq.ctx, "http://a.test/", &b) && b.len == 3 && !memcmp(b.data, "uno", 3), "read back from the file");
    /* A newer queued copy over a file on disk: the queue answers, and a
     * failed write leaves no stale file. */
    sq.store(sq.ctx, "http://a.test/", "tres", 4);
    CHECK(sq.load(sq.ctx, "http://a.test/", &b) && b.len == 4 && q.writes == 2, "the queued copy wins over the file");
    q.fail_writes = true;
    CHECK(!au_disk_cache_idle(dq, 4) && md_find(&q, n1) < 0 && !sq.load(sq.ctx, "http://a.test/", &b),
          "a failed write removes the stale file");
    q.fail_writes = false;
    /* The queue is bounded: past the limit the oldest are written at once. */
    size_t chunk = 200 * 1024;
    char *blob = malloc(chunk);
    memset(blob, 'q', chunk);
    for (int i = 0; i < 12; i++) {
        char key[32];
        snprintf(key, sizeof key, "http://q%d.test/", i);
        sq.store(sq.ctx, key, blob, chunk);
    }
    CHECK(q.writes > 2 && au_disk_cache_pending(dq) <= 10, "the queue stays under its limit: %u pending, %d writes",
          (unsigned)au_disk_cache_pending(dq), q.writes);
    free(blob);
    int w0 = q.writes;
    size_t waiting = au_disk_cache_pending(dq);
    au_disk_cache_free(dq); /* writes what is left */
    CHECK(q.writes == w0 + (int)waiting, "free flushes: %d more writes", q.writes - w0);
    for (int i = 0; i < 16; i++)
        if (q.used[i])
            au_buf_free(&q.data[i]);

    au_buf_free(&b);
    au_disk_cache_free(dc);
    au_disk_cache_free(dc2);
    for (int i = 0; i < 16; i++)
        if (m.used[i])
            au_buf_free(&m.data[i]);
}

/* ---- Through the client ---- */
typedef struct R {
    bool done, from_cache, revalidated;
    AuNetError err;
    int status;
    AuBuf body;
} R;

static void r_response(void *ctx, AuHttpTransfer *t, const AuHttpResponse *r)
{
    (void)t;
    R *x = ctx;
    x->status = r->status;
    x->from_cache = r->from_cache;
    x->revalidated = r->revalidated;
}
static void r_data(void *ctx, AuHttpTransfer *t, const void *d, size_t n)
{
    (void)t;
    au_buf_append(&((R *)ctx)->body, d, n);
}
static void r_done(void *ctx, AuHttpTransfer *t, AuNetError e)
{
    (void)t;
    ((R *)ctx)->done = true;
    ((R *)ctx)->err = e;
}

static void req_mode(FakeNet *f, AuNet *net, R *r, const char *method, const char *url, bool reload, bool history)
{
    au_buf_free(&r->body);
    memset(r, 0, sizeof *r);
    au_buf_init(&r->body);
    AuHttpRequest rq;
    au_http_request_init(&rq, url);
    rq.method = method;
    rq.reload = reload;
    rq.history = history;
    if (strcmp(method, "GET"))
        rq.body = au_body_from_bytes("a=1", 3, "application/x-www-form-urlencoded");
    AuHttpCallbacks cb = {r, r_response, r_data, r_done, NULL, NULL};
    au_http_start(net, &rq, &cb);
    au_http_request_free(&rq);
    fake_run(f, net, 500, 10);
}

static void req(FakeNet *f, AuNet *net, R *r, const char *method, const char *url, bool reload)
{
    req_mode(f, net, r, method, url, reload, false);
}

/* Back and Forward use stored responses as they are; no-store responses
 * and other sites' variants are never used. */
static void test_history(void)
{
    FakeNet f;
    fake_init(&f);
    AuNet *net = au_net_new(&f.ops);
    AuHttpCache *c = au_cache_new(1 << 20);
    au_net_set_cache(net, c);
    FakeServer *s = fake_server(&f, "h.test", 80);
    char d[32], resp[512];
    date_str(f.wall, d);
    R r;
    memset(&r, 0, sizeof r);
    au_buf_init(&r.body);

    /* no-cache with a validator: a normal load revalidates, Back does not. */
    snprintf(resp, sizeof resp,
             "HTTP/1.1 200 OK\r\nDate: %s\r\nCache-Control: no-cache\r\nETag: \"a\"\r\nContent-Length: 4\r\n\r\npage",
             d);
    fake_respond(s, resp);
    req(&f, net, &r, "GET", "http://h.test/nc", false);
    CHECK(r.err == AU_NET_OK && s->nreq == 1, "first load");
    req_mode(&f, net, &r, "GET", "http://h.test/nc", false, true);
    CHECK(r.err == AU_NET_OK && r.from_cache && !r.revalidated && s->nreq == 1 && r.body.len == 4,
          "history: no-cache page from the cache, nreq %d", s->nreq);

    /* Stale, must-revalidate: Back still uses it, an hour later. */
    snprintf(resp, sizeof resp,
             "HTTP/1.1 200 OK\r\nDate: %s\r\nCache-Control: max-age=10, must-revalidate\r\nContent-Length: 2\r\n\r\nmr",
             d);
    fake_respond(s, resp);
    req(&f, net, &r, "GET", "http://h.test/mr", false);
    f.wall += 3600;
    req_mode(&f, net, &r, "GET", "http://h.test/mr", false, true);
    CHECK(r.from_cache && s->nreq == 2, "history: stale must-revalidate page from the cache");

    /* no-store: never stored, so Back fetches it again. */
    date_str(f.wall, d);
    snprintf(resp, sizeof resp,
             "HTTP/1.1 200 OK\r\nDate: %s\r\nCache-Control: no-store\r\nContent-Length: 2\r\n\r\nns", d);
    fake_respond(s, resp);
    req(&f, net, &r, "GET", "http://h.test/ns", false);
    fake_respond(s, resp);
    req_mode(&f, net, &r, "GET", "http://h.test/ns", false, true);
    CHECK(!r.from_cache && s->nreq == 4, "history: no-store goes out, nreq %d", s->nreq);

    /* max-age=0 and no validator (Hacker News): a normal load goes out,
     * Back does not. */
    snprintf(resp, sizeof resp,
             "HTTP/1.1 200 OK\r\nDate: %s\r\nCache-Control: private; max-age=0\r\nContent-Length: 2\r\n\r\nhn", d);
    fake_respond(s, resp);
    req(&f, net, &r, "GET", "http://h.test/hn", false);
    fake_respond(s, resp);
    req(&f, net, &r, "GET", "http://h.test/hn", false);
    CHECK(!r.from_cache && s->nreq == 6, "max-age=0 without validator: a normal load goes out");
    req_mode(&f, net, &r, "GET", "http://h.test/hn", false, true);
    CHECK(r.from_cache && s->nreq == 6 && r.body.len == 2, "max-age=0 without validator: history from the cache");

    /* Not cached at all: goes out. */
    fake_respond(s, resp);
    req_mode(&f, net, &r, "GET", "http://h.test/new", false, true);
    CHECK(!r.from_cache && s->nreq == 7, "history: a miss goes out");

    /* A reload is never served from history. */
    fake_respond(s, resp);
    req_mode(&f, net, &r, "GET", "http://h.test/nc", true, true);
    CHECK(!r.from_cache && s->nreq == 8, "reload wins over history");

    au_buf_free(&r.body);
    au_net_free(net);
    au_cache_free(c);
    fake_free(&f);
}

static void test_client(void)
{
    FakeNet f;
    fake_init(&f);
    AuNet *net = au_net_new(&f.ops);
    AuHttpCache *c = au_cache_new(1 << 20);
    au_net_set_cache(net, c);
    FakeServer *s = fake_server(&f, "c.test", 80);
    char d[32], resp[512];
    date_str(f.wall, d);
    snprintf(resp, sizeof resp,
             "HTTP/1.1 200 OK\r\nDate: %s\r\nCache-Control: max-age=60\r\nETag: \"1\"\r\nContent-Type: text/plain\r\n"
             "Content-Length: 5\r\n\r\nfirst",
             d);
    fake_respond(s, resp);
    R r;
    memset(&r, 0, sizeof r);
    au_buf_init(&r.body);
    req(&f, net, &r, "GET", "http://c.test/doc#frag", false);
    CHECK(r.err == AU_NET_OK && !r.from_cache && s->nreq == 1, "first from the network");
    req(&f, net, &r, "GET", "http://c.test/doc", false);
    CHECK(r.err == AU_NET_OK && r.from_cache && r.status == 200 && s->nreq == 1 && r.body.len == 5,
          "second from the cache: from_cache %d nreq %d", r.from_cache, s->nreq);

    /* Stale: revalidated with If-None-Match, 304 delivers the cached body. */
    f.wall += 120;
    char d2[32];
    date_str(f.wall, d2);
    snprintf(resp, sizeof resp, "HTTP/1.1 304 Not Modified\r\nDate: %s\r\nCache-Control: max-age=60\r\nETag: \"1\"\r\n\r\n",
             d2);
    fake_respond(s, resp);
    req(&f, net, &r, "GET", "http://c.test/doc", false);
    CHECK(s->nreq == 2 && strstr(s->requests[1], "\r\nIf-None-Match: \"1\"\r\n"), "conditional request: %s",
          s->nreq == 2 ? s->requests[1] : "(none)");
    CHECK(r.err == AU_NET_OK && r.status == 200 && r.from_cache && r.revalidated && r.body.len == 5 &&
              !memcmp(r.body.data, "first", 5),
          "304 delivers the cached body: status %d len %zu", r.status, r.body.len);
    req(&f, net, &r, "GET", "http://c.test/doc", false);
    CHECK(r.from_cache && s->nreq == 2, "fresh again after the 304");

    /* Reload: no lookup, new response stored. */
    snprintf(resp, sizeof resp,
             "HTTP/1.1 200 OK\r\nDate: %s\r\nCache-Control: max-age=60\r\nContent-Length: 6\r\n\r\nsecond", d2);
    fake_respond(s, resp);
    req(&f, net, &r, "GET", "http://c.test/doc", true);
    CHECK(s->nreq == 3 && !r.from_cache && strstr(s->requests[2], "Cache-Control: no-cache"), "reload goes out");
    req(&f, net, &r, "GET", "http://c.test/doc", false);
    CHECK(r.from_cache && r.body.len == 6, "reloaded response cached");

    /* POST invalidates. */
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    req(&f, net, &r, "POST", "http://c.test/doc", false);
    CHECK(r.err == AU_NET_OK && !r.from_cache, "post");
    snprintf(resp, sizeof resp,
             "HTTP/1.1 200 OK\r\nDate: %s\r\nCache-Control: no-store\r\nContent-Length: 5\r\n\r\nthird", d2);
    fake_respond(s, resp);
    req(&f, net, &r, "GET", "http://c.test/doc", false);
    CHECK(!r.from_cache && s->nreq == 5, "after POST the GET goes out");
    fake_respond(s, resp);
    req(&f, net, &r, "GET", "http://c.test/doc", false);
    CHECK(!r.from_cache && s->nreq == 6, "no-store was not cached");

    /* A gzip body is stored decoded. */
    static const unsigned char gz[] = {0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xcb, 0x48,
                                       0xcd, 0xc9, 0xc9, 0x07, 0x00, 0x86, 0xa6, 0x10, 0x36, 0x05, 0x00, 0x00, 0x00};
    char *gzresp = malloc(512);
    int hl = snprintf(gzresp, 512,
                      "HTTP/1.1 200 OK\r\nDate: %s\r\nCache-Control: max-age=60\r\nContent-Encoding: gzip\r\n"
                      "Content-Length: %zu\r\n\r\n",
                      d2, sizeof gz);
    memcpy(gzresp + hl, gz, sizeof gz);
    FakeResponse *g = fake_add(s, gzresp);
    g->len = (size_t)hl + sizeof gz;
    req(&f, net, &r, "GET", "http://c.test/gz", false);
    CHECK(r.err == AU_NET_OK && r.body.len == 5 && !memcmp(r.body.data, "hello", 5), "gzip decoded");
    req(&f, net, &r, "GET", "http://c.test/gz", false);
    CHECK(r.from_cache && r.body.len == 5 && !memcmp(r.body.data, "hello", 5), "cached decoded");
    free(gzresp);

    au_buf_free(&r.body);
    au_net_free(net);
    au_cache_free(c);
    fake_free(&f);
}

int main(void)
{
    test_control();
    test_freshness();
    test_store_lookup();
    test_limits();
    test_storage();
    test_disk_cache();
    test_client();
    test_history();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
