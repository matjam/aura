/* The HTTP cache (see cache.h). */
#include "cache.h"

#include <limits.h>
#include <string.h>

#define NBUCKETS 1024
#define ENTRY_OVERHEAD 256          /* bookkeeping counted per entry */
#define ENTRY_LIMIT_MAX (8u << 20)  /* largest body kept */
#define HEURISTIC_CAP 86400         /* one day */
#define SER_MAX_FIELDS 512
#define SER_MAX_STRING (1u << 20)
#define SER_MAX_BODY (64u << 20)

struct AuCacheEntry {
    AuCacheEntry *prev, *next; /* LRU list: head is the most recently used */
    AuCacheEntry *hnext;       /* hash chain */
    char *url;
    uint32_t hash;
    int status;
    char *reason;
    AuHeaders resp;
    /* The request's values of the Vary fields: value "=x" when present,
     * "!" when absent. */
    AuHeaders vary;
    uint8_t *body;
    size_t len;
    int64_t request_time, response_time;
    int refs;
    bool linked;
    size_t cost;
};

struct AuCacheWriter {
    AuHttpCache *cache;
    AuCacheEntry *e;
    AuBuf body;
};

struct AuHttpCache {
    AuCacheEntry *buckets[NBUCKETS];
    AuCacheEntry *head, *tail;
    size_t bytes, count, max;
    AuCacheStorage storage;
    bool have_storage;
    AuCacheStats stats;
};

void au_cache_stats(const AuHttpCache *c, AuCacheStats *out) { *out = c->stats; }

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static char *dupz(const char *s) { return au_strndup(s, strlen(s)); }

AuHttpCache *au_cache_new(size_t max_bytes)
{
    AuHttpCache *c = au_calloc(sizeof *c);
    c->max = max_bytes;
    return c;
}

void au_cache_entry_free(AuCacheEntry *e)
{
    if (!e)
        return;
    au_free(e->url);
    au_free(e->reason);
    au_headers_free(&e->resp);
    au_headers_free(&e->vary);
    au_free(e->body);
    au_free(e);
}

static void unlink_entry(AuHttpCache *c, AuCacheEntry *e)
{
    if (!e->linked)
        return;
    if (e->prev)
        e->prev->next = e->next;
    else
        c->head = e->next;
    if (e->next)
        e->next->prev = e->prev;
    else
        c->tail = e->prev;
    AuCacheEntry **pp = &c->buckets[e->hash % NBUCKETS];
    while (*pp && *pp != e)
        pp = &(*pp)->hnext;
    if (*pp)
        *pp = e->hnext;
    e->prev = e->next = e->hnext = NULL;
    e->linked = false;
    c->bytes -= e->cost;
    c->count--;
}

static void drop(AuHttpCache *c, AuCacheEntry *e)
{
    unlink_entry(c, e);
    if (e->refs == 0)
        au_cache_entry_free(e);
}

static void touch(AuHttpCache *c, AuCacheEntry *e)
{
    if (!e->linked || c->head == e)
        return;
    e->prev->next = e->next;
    if (e->next)
        e->next->prev = e->prev;
    else
        c->tail = e->prev;
    e->prev = NULL;
    e->next = c->head;
    c->head->prev = e;
    c->head = e;
}

static void evict_to(AuHttpCache *c, size_t max)
{
    while (c->bytes > max && c->tail)
        drop(c, c->tail);
}

void au_cache_free(AuHttpCache *c)
{
    if (!c)
        return;
    au_cache_clear(c);
    au_free(c);
}

void au_cache_set_max(AuHttpCache *c, size_t max_bytes)
{
    c->max = max_bytes;
    evict_to(c, max_bytes);
}

void au_cache_set_storage(AuHttpCache *c, const AuCacheStorage *s)
{
    c->have_storage = s != NULL;
    if (s)
        c->storage = *s;
}

void au_cache_clear(AuHttpCache *c)
{
    while (c->head)
        drop(c, c->head);
}

size_t au_cache_bytes(const AuHttpCache *c) { return c->bytes; }
size_t au_cache_count(const AuHttpCache *c) { return c->count; }

size_t au_cache_entry_limit(const AuHttpCache *c)
{
    /* A quarter of the budget: a Wikipedia article's HTML (1.3 MB for
     * "Mac (computer)") did not fit an eighth of the 4 MB default, so Back
     * fetched it again. */
    size_t l = c->max / 4;
    return l > ENTRY_LIMIT_MAX ? ENTRY_LIMIT_MAX : l;
}

/* ---- Cache-Control ---- */

static bool parse_seconds(const char *v, size_t n, int64_t *out)
{
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
        v++;
        n -= 2;
    }
    if (!n)
        return false;
    int64_t x = 0;
    for (size_t i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9')
            return false;
        if (x < (int64_t)1 << 40) /* clamp: about 34 000 years */
            x = x * 10 + (v[i] - '0');
    }
    *out = x;
    return true;
}

void au_cache_control_parse(const AuHeaders *h, AuCacheControl *cc)
{
    memset(cc, 0, sizeof *cc);
    cc->max_age = cc->s_maxage = cc->max_stale = cc->min_fresh = -1;
    size_t pos = 0;
    const char *v;
    while ((v = au_headers_next(h, "cache-control", &pos)) != NULL) {
        const char *s = v, *item;
        size_t il;
        while (au_http_list_next(&s, &item, &il)) {
            size_t nl = 0;
            while (nl < il && item[nl] != '=')
                nl++;
            const char *val = nl < il ? item + nl + 1 : NULL;
            size_t vl = val ? il - nl - 1 : 0;
            while (nl && (item[nl - 1] == ' ' || item[nl - 1] == '\t'))
                nl--;
            while (val && vl && (*val == ' ' || *val == '\t')) {
                val++;
                vl--;
            }
            int64_t secs;
            if (au_ieq(item, nl, "no-store", 8))
                cc->no_store = true;
            else if (au_ieq(item, nl, "no-cache", 8))
                cc->no_cache = true; /* with or without a field list: revalidate */
            else if (au_ieq(item, nl, "must-revalidate", 15) || au_ieq(item, nl, "proxy-revalidate", 16))
                cc->must_revalidate = true;
            else if (au_ieq(item, nl, "public", 6))
                cc->public_ = true;
            else if (au_ieq(item, nl, "private", 7))
                cc->private_ = true;
            else if (au_ieq(item, nl, "immutable", 9))
                cc->immutable = true;
            else if (au_ieq(item, nl, "max-age", 7) && val && parse_seconds(val, vl, &secs))
                cc->max_age = cc->max_age < 0 ? secs : (secs < cc->max_age ? secs : cc->max_age);
            else if (au_ieq(item, nl, "s-maxage", 8) && val && parse_seconds(val, vl, &secs))
                cc->s_maxage = secs;
            else if (au_ieq(item, nl, "max-stale", 9))
                cc->max_stale = (val && parse_seconds(val, vl, &secs)) ? secs : INT64_MAX;
            else if (au_ieq(item, nl, "min-fresh", 9) && val && parse_seconds(val, vl, &secs))
                cc->min_fresh = secs;
        }
    }
}

static bool header_time(const AuHeaders *h, const char *name, int64_t *t)
{
    const char *v = au_headers_get(h, name);
    return v && au_http_parse_date(v, t);
}

int64_t au_cache_freshness_lifetime(int status, const AuHeaders *resp)
{
    AuCacheControl cc;
    au_cache_control_parse(resp, &cc);
    if (cc.max_age >= 0)
        return cc.max_age;
    int64_t date, exp, lm;
    bool have_date = header_time(resp, "date", &date);
    if (au_headers_get(resp, "expires")) {
        if (!header_time(resp, "expires", &exp) || !have_date)
            return 0; /* an invalid Expires means already expired */
        return exp > date ? exp - date : 0;
    }
    /* Heuristic freshness (RFC 9111 4.2.2) for the heuristically
     * cacheable statuses. */
    switch (status) {
    case 200: case 203: case 204: case 300: case 301: case 308:
    case 404: case 405: case 410: case 414: case 501:
        break;
    default:
        return 0;
    }
    if (have_date && header_time(resp, "last-modified", &lm) && date > lm) {
        int64_t h = (date - lm) / 10;
        return h > HEURISTIC_CAP ? HEURISTIC_CAP : h;
    }
    return 0;
}

int64_t au_cache_current_age(const AuHeaders *resp, int64_t request_time, int64_t response_time, int64_t now)
{
    int64_t date, age_value = 0;
    if (!header_time(resp, "date", &date))
        date = response_time;
    const char *a = au_headers_get(resp, "age");
    if (a) {
        int64_t x;
        if (parse_seconds(a, strlen(a), &x))
            age_value = x;
    }
    int64_t apparent = response_time - date;
    if (apparent < 0)
        apparent = 0;
    int64_t delay = response_time - request_time;
    if (delay < 0)
        delay = 0;
    int64_t corrected_age = age_value + delay;
    int64_t initial = apparent > corrected_age ? apparent : corrected_age;
    int64_t resident = now - response_time;
    if (resident < 0)
        resident = 0;
    return initial + resident;
}

/* ---- Entries ---- */

int au_cache_entry_status(const AuCacheEntry *e) { return e->status; }
const char *au_cache_entry_reason(const AuCacheEntry *e) { return e->reason; }
const AuHeaders *au_cache_entry_headers(const AuCacheEntry *e) { return &e->resp; }

const uint8_t *au_cache_entry_body(const AuCacheEntry *e, size_t *len)
{
    *len = e->len;
    return e->body;
}

const char *au_cache_entry_etag(const AuCacheEntry *e) { return au_headers_get(&e->resp, "etag"); }
const char *au_cache_entry_last_modified(const AuCacheEntry *e) { return au_headers_get(&e->resp, "last-modified"); }

bool au_cache_entry_immutable(const AuCacheEntry *e)
{
    AuCacheControl cc;
    au_cache_control_parse(&e->resp, &cc);
    return cc.immutable;
}

void au_cache_entry_release(AuHttpCache *c, AuCacheEntry *e)
{
    (void)c;
    if (!e)
        return;
    if (--e->refs == 0 && !e->linked)
        au_cache_entry_free(e);
}

/* Fields never kept: hop-by-hop, framing and codings, cookies. */
static bool keep_field(const char *name)
{
    static const char *drop_names[] = {"connection", "keep-alive", "proxy-connection", "te", "trailer",
                                       "transfer-encoding", "upgrade", "content-encoding", "content-length",
                                       "set-cookie", "set-cookie2", "proxy-authenticate", "proxy-authorization"};
    for (size_t i = 0; i < sizeof drop_names / sizeof *drop_names; i++)
        if (au_ieq(name, strlen(name), drop_names[i], strlen(drop_names[i])))
            return false;
    return true;
}

static size_t headers_cost(const AuHeaders *h)
{
    size_t n = 0;
    for (size_t i = 0; i < h->n; i++)
        n += strlen(h->items[i].name) + strlen(h->items[i].value) + 16;
    return n;
}

static void compute_cost(AuCacheEntry *e)
{
    e->cost = ENTRY_OVERHEAD + strlen(e->url) + strlen(e->reason) + headers_cost(&e->resp) + headers_cost(&e->vary) +
              e->len;
}

/* Records the request's values of the response's Vary fields. Returns
 * false for Vary: *. */
static bool record_vary(const AuHeaders *resp, const AuHeaders *req, AuHeaders *out)
{
    au_headers_init(out);
    size_t pos = 0;
    const char *v;
    while ((v = au_headers_next(resp, "vary", &pos)) != NULL) {
        const char *s = v, *item;
        size_t il;
        while (au_http_list_next(&s, &item, &il)) {
            if (il == 1 && item[0] == '*') {
                au_headers_free(out);
                return false;
            }
            char name[64];
            if (il >= sizeof name)
                continue;
            for (size_t i = 0; i < il; i++)
                name[i] = (char)au_lower((unsigned char)item[i]);
            name[il] = 0;
            if (au_headers_get(out, name))
                continue;
            AuBuf val;
            au_buf_init(&val);
            if (req && au_headers_get_combined(req, name, &val)) {
                AuBuf pv;
                au_buf_init(&pv);
                au_buf_putc(&pv, '=');
                au_buf_append(&pv, val.data, val.len);
                au_headers_add(out, name, au_buf_cstr(&pv));
                au_buf_free(&pv);
            } else {
                au_headers_add(out, name, "!");
            }
            au_buf_free(&val);
        }
    }
    return true;
}

static bool vary_matches(const AuCacheEntry *e, const AuHeaders *req)
{
    for (size_t i = 0; i < e->vary.n; i++) {
        const char *want = e->vary.items[i].value;
        AuBuf val;
        au_buf_init(&val);
        bool present = req && au_headers_get_combined(req, e->vary.items[i].name, &val);
        bool ok = present ? (want[0] == '=' && strlen(want + 1) == val.len && !memcmp(want + 1, val.data, val.len))
                          : want[0] == '!';
        au_buf_free(&val);
        if (!ok)
            return false;
    }
    return true;
}

static bool same_variant(const AuCacheEntry *a, const AuCacheEntry *b)
{
    if (a->vary.n != b->vary.n)
        return false;
    for (size_t i = 0; i < a->vary.n; i++) {
        const char *v = au_headers_get(&b->vary, a->vary.items[i].name);
        if (!v || strcmp(v, a->vary.items[i].value))
            return false;
    }
    return true;
}

static void insert(AuHttpCache *c, AuCacheEntry *e)
{
    e->hash = hash_str(e->url);
    compute_cost(e);
    /* Replace the same variant. */
    for (AuCacheEntry *x = c->buckets[e->hash % NBUCKETS], *nx; x; x = nx) {
        nx = x->hnext;
        if (x->hash == e->hash && !strcmp(x->url, e->url) && same_variant(x, e))
            drop(c, x);
    }
    e->hnext = c->buckets[e->hash % NBUCKETS];
    c->buckets[e->hash % NBUCKETS] = e;
    e->prev = NULL;
    e->next = c->head;
    if (c->head)
        c->head->prev = e;
    c->head = e;
    if (!c->tail)
        c->tail = e;
    e->linked = true;
    c->bytes += e->cost;
    c->count++;
    evict_to(c, c->max);
}

static bool has_validator(const AuCacheEntry *e)
{
    return au_cache_entry_etag(e) || au_cache_entry_last_modified(e);
}

static void storage_put(AuHttpCache *c, const AuCacheEntry *e)
{
    if (!c->have_storage || !c->storage.store)
        return;
    AuBuf b;
    au_buf_init(&b);
    au_cache_entry_serialize(e, &b);
    c->storage.store(c->storage.ctx, e->url, b.data, b.len);
    au_buf_free(&b);
}

static AuCacheEntry *find_entry(AuHttpCache *c, const char *url, const AuHeaders *req);

AuCacheState au_cache_lookup(AuHttpCache *c, const char *url, const AuHeaders *req, int64_t now, AuCacheEntry **out)
{
    *out = NULL;
    AuCacheControl rcc;
    au_cache_control_parse(req, &rcc);
    if (rcc.no_store)
        return AU_CACHE_MISS;
    AuCacheEntry *best = find_entry(c, url, req);
    if (!best) {
        c->stats.misses++;
        return AU_CACHE_MISS;
    }
    touch(c, best);
    AuCacheControl cc;
    au_cache_control_parse(&best->resp, &cc);
    bool pragma_no_cache = !au_headers_get(req, "cache-control") && au_headers_has_token(req, "pragma", "no-cache");
    int64_t lifetime = au_cache_freshness_lifetime(best->status, &best->resp);
    int64_t age = au_cache_current_age(&best->resp, best->request_time, best->response_time, now);
    bool fresh = lifetime > age;
    if (rcc.max_age >= 0 && age > rcc.max_age)
        fresh = false;
    if (rcc.min_fresh >= 0 && lifetime - age < rcc.min_fresh)
        fresh = false;
    if (!fresh && !cc.must_revalidate && !cc.no_cache && rcc.max_stale >= 0 &&
        (rcc.max_stale == INT64_MAX || age - lifetime <= rcc.max_stale))
        fresh = true;
    if (cc.no_cache || rcc.no_cache || pragma_no_cache)
        fresh = false;
    if (!fresh && !has_validator(best)) {
        c->stats.misses++;
        return AU_CACHE_MISS;
    }
    best->refs++;
    *out = best;
    if (fresh)
        c->stats.fresh++;
    else
        c->stats.stale++;
    return fresh ? AU_CACHE_FRESH : AU_CACHE_STALE;
}

AuCacheState au_cache_lookup_history(AuHttpCache *c, const char *url, const AuHeaders *req, AuCacheEntry **out)
{
    *out = NULL;
    AuCacheControl rcc;
    au_cache_control_parse(req, &rcc);
    if (rcc.no_store)
        return AU_CACHE_MISS;
    AuCacheEntry *best = find_entry(c, url, req);
    if (!best) {
        c->stats.misses++;
        return AU_CACHE_MISS;
    }
    touch(c, best);
    best->refs++;
    *out = best;
    c->stats.history++;
    return AU_CACHE_FRESH;
}

/* The newest entry for url whose Vary fields match req, from memory or
 * the second-level storage. */
static AuCacheEntry *find_entry(AuHttpCache *c, const char *url, const AuHeaders *req)
{
    uint32_t h = hash_str(url);
    AuCacheEntry *best = NULL;
    for (AuCacheEntry *x = c->buckets[h % NBUCKETS]; x; x = x->hnext)
        if (x->hash == h && !strcmp(x->url, url) && vary_matches(x, req) &&
            (!best || x->response_time > best->response_time))
            best = x;
    if (!best && c->have_storage && c->storage.load) {
        AuBuf b;
        au_buf_init(&b);
        if (c->storage.load(c->storage.ctx, url, &b)) {
            AuCacheEntry *e = au_cache_entry_parse(b.data, b.len);
            if (e && !strcmp(e->url, url) && e->len <= au_cache_entry_limit(c)) {
                insert(c, e);
                if (e->linked && vary_matches(e, req)) {
                    best = e;
                    c->stats.from_storage++;
                }
            } else {
                au_cache_entry_free(e);
                if (c->storage.remove)
                    c->storage.remove(c->storage.ctx, url);
            }
        }
        au_buf_free(&b);
    }
    return best;
}

AuCacheWriter *au_cache_begin(AuHttpCache *c, const char *url, const AuHeaders *req, int status, const char *reason,
                              const AuHeaders *resp, int64_t request_time, int64_t response_time)
{
    switch (status) {
    case 200: case 203: case 204: case 300: case 301: case 308:
    case 404: case 405: case 410: case 414: case 501:
        break;
    default:
        return NULL;
    }
    if (c->max == 0)
        return NULL;
    AuCacheControl cc, rcc;
    au_cache_control_parse(resp, &cc);
    au_cache_control_parse(req, &rcc);
    if (cc.no_store || rcc.no_store)
        return NULL;
    if (req && au_headers_get(req, "authorization") && !cc.public_ && !cc.must_revalidate && cc.s_maxage < 0)
        return NULL;
    AuCacheEntry *e = au_calloc(sizeof *e);
    if (!record_vary(resp, req, &e->vary)) {
        au_free(e);
        return NULL;
    }
    au_headers_init(&e->resp);
    for (size_t i = 0; i < resp->n; i++)
        if (keep_field(resp->items[i].name))
            au_headers_add(&e->resp, resp->items[i].name, resp->items[i].value);
    if (!au_headers_get(&e->resp, "date")) {
        char d[32];
        au_http_format_date(response_time, d);
        au_headers_add(&e->resp, "Date", d);
    }
    e->url = dupz(url);
    e->status = status;
    e->reason = dupz(reason ? reason : "");
    e->request_time = request_time;
    e->response_time = response_time;
    /* Kept even when it can be neither reused nor revalidated (no
     * freshness, no validator, as Hacker News pages are): a normal lookup
     * misses it, and Back shows it (au_cache_lookup_history). */
    AuCacheWriter *w = au_calloc(sizeof *w);
    w->cache = c;
    w->e = e;
    au_buf_init(&w->body);
    return w;
}

bool au_cache_write(AuCacheWriter *w, const void *data, size_t len)
{
    if (len > au_cache_entry_limit(w->cache) - w->body.len || w->body.len > au_cache_entry_limit(w->cache)) {
        au_cache_abort(w);
        return false;
    }
    au_buf_append(&w->body, data, len);
    return true;
}

void au_cache_abort(AuCacheWriter *w)
{
    if (!w)
        return;
    au_cache_entry_free(w->e);
    au_buf_free(&w->body);
    au_free(w);
}

void au_cache_commit(AuCacheWriter *w)
{
    AuHttpCache *c = w->cache;
    AuCacheEntry *e = w->e;
    e->body = (uint8_t *)au_buf_take(&w->body, &e->len);
    au_free(w);
    insert(c, e);
    c->stats.stored++;
    if (e->linked)
        storage_put(c, e);
}

void au_cache_update(AuHttpCache *c, AuCacheEntry *e, const AuHeaders *resp304, int64_t request_time,
                     int64_t response_time)
{
    bool linked = e->linked;
    c->stats.not_modified++;
    if (linked) {
        c->bytes -= e->cost;
    }
    for (size_t i = 0; i < resp304->n; i++) {
        const char *n = resp304->items[i].name;
        if (!keep_field(n))
            continue;
        au_headers_remove(&e->resp, n);
    }
    for (size_t i = 0; i < resp304->n; i++)
        if (keep_field(resp304->items[i].name))
            au_headers_add(&e->resp, resp304->items[i].name, resp304->items[i].value);
    if (!au_headers_get(resp304, "date")) {
        char d[32];
        au_http_format_date(response_time, d);
        au_headers_set(&e->resp, "Date", d);
    }
    au_headers_remove(&e->resp, "age");
    const char *age = au_headers_get(resp304, "age");
    if (age)
        au_headers_add(&e->resp, "Age", age);
    e->request_time = request_time;
    e->response_time = response_time;
    compute_cost(e);
    if (linked) {
        c->bytes += e->cost;
        touch(c, e);
        storage_put(c, e);
        evict_to(c, c->max);
    }
}

void au_cache_invalidate(AuHttpCache *c, const char *url)
{
    uint32_t h = hash_str(url);
    for (AuCacheEntry *x = c->buckets[h % NBUCKETS], *nx; x; x = nx) {
        nx = x->hnext;
        if (x->hash == h && !strcmp(x->url, url))
            drop(c, x);
    }
    if (c->have_storage && c->storage.remove)
        c->storage.remove(c->storage.ctx, url);
}

/* ---- Serialization ---- */

static void put32(AuBuf *b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    au_buf_append(b, x, 4);
}

static void put64(AuBuf *b, int64_t v)
{
    put32(b, (uint32_t)((uint64_t)v >> 32));
    put32(b, (uint32_t)v);
}

static void put_str(AuBuf *b, const char *s)
{
    size_t n = strlen(s);
    put32(b, (uint32_t)n);
    au_buf_append(b, s, n);
}

static void put_headers(AuBuf *b, const AuHeaders *h)
{
    put32(b, (uint32_t)h->n);
    for (size_t i = 0; i < h->n; i++) {
        put_str(b, h->items[i].name);
        put_str(b, h->items[i].value);
    }
}

void au_cache_entry_serialize(const AuCacheEntry *e, AuBuf *out)
{
    au_buf_puts(out, "AURC1\n");
    put32(out, (uint32_t)e->status);
    put_str(out, e->reason);
    put_str(out, e->url);
    put64(out, e->request_time);
    put64(out, e->response_time);
    put_headers(out, &e->vary);
    put_headers(out, &e->resp);
    put32(out, (uint32_t)e->len);
    au_buf_append(out, e->body, e->len);
}

typedef struct Rd {
    const uint8_t *p;
    size_t n, off;
    bool bad;
} Rd;

static uint32_t get32(Rd *r)
{
    if (r->bad || r->n - r->off < 4) {
        r->bad = true;
        return 0;
    }
    const uint8_t *p = r->p + r->off;
    r->off += 4;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int64_t get64(Rd *r)
{
    uint64_t hi = get32(r), lo = get32(r);
    return (int64_t)(hi << 32 | lo);
}

static char *get_str(Rd *r)
{
    uint32_t n = get32(r);
    if (r->bad || n > SER_MAX_STRING || n > r->n - r->off) {
        r->bad = true;
        return NULL;
    }
    char *s = au_strndup((const char *)r->p + r->off, n);
    if (memchr(s, 0, n))
        r->bad = true; /* embedded NUL */
    r->off += n;
    return s;
}

static void get_headers(Rd *r, AuHeaders *h)
{
    au_headers_init(h);
    uint32_t n = get32(r);
    if (n > SER_MAX_FIELDS) {
        r->bad = true;
        return;
    }
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        char *name = get_str(r);
        char *value = get_str(r);
        if (!r->bad)
            au_headers_add(h, name, value);
        au_free(name);
        au_free(value);
    }
}

AuCacheEntry *au_cache_entry_parse(const void *data, size_t len)
{
    Rd r = {data, len, 0, false};
    if (len < 6 || memcmp(data, "AURC1\n", 6))
        return NULL;
    r.off = 6;
    AuCacheEntry *e = au_calloc(sizeof *e);
    au_headers_init(&e->vary);
    au_headers_init(&e->resp);
    e->status = (int)get32(&r);
    e->reason = get_str(&r);
    e->url = get_str(&r);
    e->request_time = get64(&r);
    e->response_time = get64(&r);
    au_headers_free(&e->vary);
    get_headers(&r, &e->vary);
    au_headers_free(&e->resp);
    get_headers(&r, &e->resp);
    uint32_t bl = get32(&r);
    if (!r.bad && (bl > SER_MAX_BODY || bl != r.n - r.off))
        r.bad = true;
    if (!r.bad) {
        e->len = bl;
        e->body = au_alloc(bl ? bl : 1);
        memcpy(e->body, r.p + r.off, bl);
    }
    if (r.bad || e->status < 100 || e->status > 999 || !e->url || !e->reason) {
        au_cache_entry_free(e);
        return NULL;
    }
    return e;
}
