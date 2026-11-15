/* The blocklist catalogue, store and updates (see blocklist_store.h). */
#include "blocklist_store.h"
#include "headers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CUSTOM 8
#define MAX_LISTS 16
#define MAX_TEXT_BYTES (96u * 1024 * 1024) /* a list's text */
#define MERGE_SLICE 20000u                /* entries merged per idle call */
#define RETRY_MISSING (10 * 60)           /* seconds before a missing list is tried again */
#define RETRY_UPDATE (6 * 3600)           /* and a list that has an old copy */
#define TIMEOUT_MS (15u * 60 * 1000)

/* HaGeZi's DNS blocklists in the "wildcard domains" format: one domain
 * per line with subdomains removed, the smallest form to fetch and the
 * cheapest to parse. The URLs are the ones the project's README gives
 * (jsDelivr over the GitHub repository). The lists are GPL-3.0 and are
 * not part of Aura: they are fetched from the publisher on the user's
 * machine. */
#define HAGEZI "https://cdn.jsdelivr.net/gh/hagezi/dns-blocklists@latest/wildcard/"
static const AuBlockCatalogEntry catalog[] = {
    {"light", "HaGeZi Light", HAGEZI "light-onlydomains.txt",
     "Ads, trackers and telemetry on widely used domains. Should break nothing.", false, 91000},
    {"normal", "HaGeZi Normal", HAGEZI "multi-onlydomains.txt",
     "Ads, trackers, telemetry, phishing and malware. Rarely breaks a site.", false, 223000},
    {"pro", "HaGeZi Pro", HAGEZI "pro-onlydomains.txt",
     "Normal plus more tracking and error reporting domains. May break a few sites.", false, 237000},
    {"proplus", "HaGeZi Pro++", HAGEZI "pro.plus-onlydomains.txt",
     "The most thorough of the four. Expect to allow a site now and then.", false, 260000},
    {"tif-mini", "HaGeZi Threat Intelligence Feeds (Mini)", HAGEZI "tif.mini-onlydomains.txt",
     "Malware, phishing and scam domains found in the top million. 1.1 MB of memory.", true, 183000},
    {"tif-medium", "HaGeZi Threat Intelligence Feeds (Medium)", HAGEZI "tif.medium-onlydomains.txt",
     "A larger selection of the threat feeds. 2.1 MB of memory.", true, 353000},
    {"tif", "HaGeZi Threat Intelligence Feeds", HAGEZI "tif-onlydomains.txt",
     "Every threat feed: a 27 MB download and 8.7 MB of memory.", true, 1455000},
};
#define NCATALOG (sizeof catalog / sizeof catalog[0])

const AuBlockCatalogEntry *au_block_catalog(size_t *n)
{
    *n = NCATALOG;
    return catalog;
}

typedef struct MList {
    char id[32];
    char title[AU_BLOCK_TITLE_MAX];
    char *url; /* owned for lists added by URL */
    const AuBlockCatalogEntry *cat;
    bool enabled, loaded, force;
    bool tried_store; /* the store was asked for the compiled file */
    uint32_t entries, bytes;
    int64_t updated_at, checked_at, retry_at;
    char etag[160], last_modified[64];
    char error[96];
} MList;

struct AuBlockManager {
    AuNet *net;
    AuBlocklist *bl;
    AuBlockStore store;
    AuBlockMem mem;
    bool have_mem;
    MList lists[MAX_LISTS];
    size_t nlists;
    bool on;
    int interval; /* seconds; 0 never */
    bool index_loaded, index_dirty;
    uint32_t generation;
    /* the download in progress */
    int cur; /* index into lists, -1 none */
    AuHttpTransfer *t;
    AuBlockBuilder *builder;
    AuBlockListState state;
    int64_t received;
    bool got_200, got_304, failed;
    char new_etag[160], new_modified[64];
    char fail_text[96];
    uint32_t t_start, t_merge, merge_calls;
    AuBlockCompileStats last;
};

static const AuBlockMem *mem_of(const AuBlockManager *m) { return m->have_mem ? &m->mem : NULL; }
static int64_t wall(AuBlockManager *m)
{
    const AuNetOps *o = au_net_ops(m->net);
    return o->wall_time(o->ctx);
}
static uint32_t ticks(AuBlockManager *m)
{
    const AuNetOps *o = au_net_ops(m->net);
    return o->now_ms(o->ctx);
}

static void release_block(AuBlockManager *m, uint8_t *p)
{
    if (!p)
        return;
    if (m->have_mem)
        m->mem.release(m->mem.ctx, p);
    else
        au_free(p);
}

AuBlockManager *au_block_manager_new(AuNet *net, AuBlocklist *bl, const AuBlockStore *store, const AuBlockMem *mem)
{
    AuBlockManager *m = au_calloc(sizeof *m);
    m->net = net;
    m->bl = bl;
    m->store = *store;
    if (mem && mem->alloc) {
        m->mem = *mem;
        m->have_mem = true;
    }
    m->cur = -1;
    m->interval = 86400;
    for (size_t i = 0; i < NCATALOG; i++) {
        MList *l = &m->lists[m->nlists++];
        snprintf(l->id, sizeof l->id, "%s", catalog[i].id);
        snprintf(l->title, sizeof l->title, "%s", catalog[i].title);
        l->cat = &catalog[i];
    }
    return m;
}

static void drop_transfer(AuBlockManager *m)
{
    if (m->t)
        au_http_cancel(m->t);
    m->t = NULL;
    au_block_builder_free(m->builder);
    m->builder = NULL;
    m->cur = -1;
    m->state = AU_BLOCK_IDLE;
}

void au_block_manager_free(AuBlockManager *m)
{
    if (!m)
        return;
    drop_transfer(m);
    for (size_t i = 0; i < m->nlists; i++)
        au_free(m->lists[i].url);
    au_free(m);
}

static const char *list_url(const MList *l) { return l->cat ? l->cat->url : l->url; }
static void save_index(AuBlockManager *m);

/* ---- The index file ---- */

static void clean_field(char *dst, size_t cap, const char *s, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c < 0x7F)
            dst[k++] = (char)c;
    }
    dst[k] = 0;
}

static int64_t parse_i64(const char *s, size_t n)
{
    int64_t v = 0;
    for (size_t i = 0; i < n && i < 18; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static MList *find_list(AuBlockManager *m, const char *id)
{
    for (size_t i = 0; i < m->nlists; i++)
        if (!strcmp(m->lists[i].id, id))
            return &m->lists[i];
    return NULL;
}

/* Lines of "id TAB checked TAB updated TAB etag TAB last-modified". */
static void load_index(AuBlockManager *m)
{
    m->index_loaded = true;
    uint8_t *data = NULL;
    size_t len = 0;
    if (!m->store.read || !m->store.read(m->store.ctx, "Index", NULL, &data, &len))
        return;
    const char *p = (const char *)data, *end = p + (len > 65536 ? 65536 : len);
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        const char *f[5];
        size_t fl[5];
        int nf = 0;
        for (const char *q = p; nf < 5;) {
            const char *tab = memchr(q, '\t', (size_t)(le - q));
            f[nf] = q;
            fl[nf++] = (size_t)((tab ? tab : le) - q);
            if (!tab)
                break;
            q = tab + 1;
        }
        if (nf == 5 && fl[0] && fl[0] < 32) {
            char id[32];
            clean_field(id, sizeof id, f[0], fl[0]);
            MList *l = find_list(m, id);
            if (l) {
                l->checked_at = parse_i64(f[1], fl[1]);
                l->updated_at = parse_i64(f[2], fl[2]);
                clean_field(l->etag, sizeof l->etag, f[3], fl[3]);
                clean_field(l->last_modified, sizeof l->last_modified, f[4], fl[4]);
            }
        }
        p = le + 1;
    }
    au_free(data);
}

static void save_index(AuBlockManager *m)
{
    m->index_dirty = false;
    if (!m->store.write)
        return;
    AuBuf b;
    au_buf_init(&b);
    for (size_t i = 0; i < m->nlists; i++) {
        const MList *l = &m->lists[i];
        if (!l->checked_at && !l->updated_at)
            continue;
        au_buf_puts(&b, l->id);
        au_buf_putc(&b, '\t');
        au_put_i64(&b, l->checked_at);
        au_buf_putc(&b, '\t');
        au_put_i64(&b, l->updated_at);
        au_buf_printf(&b, "\t%s\t%s\n", l->etag, l->last_modified);
    }
    m->store.write(m->store.ctx, "Index", b.data ? b.data : "", b.len);
    au_buf_free(&b);
}

/* ---- Preferences ---- */

static uint32_t fnv32s(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h;
}

static bool https_url(const char *s, size_t n)
{
    if (n < 12 || n > 1024 || !au_ieq(s, 8, "https://", 8))
        return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] <= ' ' || (unsigned char)s[i] >= 0x7F)
            return false;
    return true;
}

/* Attaches l's compiled file from the store, once. */
static void load_from_store(AuBlockManager *m, MList *l)
{
    if (l->loaded || l->tried_store)
        return;
    l->tried_store = true;
    uint8_t *data = NULL;
    size_t len = 0;
    if (!m->store.read || !m->store.read(m->store.ctx, l->id, mem_of(m), &data, &len))
        return;
    /* A damaged file is refused (and released) by the check inside. */
    if (!au_blocklist_attach(m->bl, l->id, data, len, mem_of(m))) {
        snprintf(l->error, sizeof l->error, "the saved list was damaged");
        return;
    }
    AuBlockFileInfo info;
    au_blocklist_has(m->bl, l->id, &info);
    l->loaded = true;
    l->entries = info.blocks;
    l->bytes = (uint32_t)len;
    if (!l->cat && info.title[0])
        snprintf(l->title, sizeof l->title, "%s", info.title);
}

static void unload(AuBlockManager *m, MList *l)
{
    if (l->loaded)
        au_blocklist_detach(m->bl, l->id);
    l->loaded = false;
    l->tried_store = false;
    l->bytes = 0;
    l->entries = 0;
}

void au_block_manager_apply_prefs(AuBlockManager *m, const AuPrefs *p)
{
    if (!m->index_loaded)
        load_index(m);
    m->on = p->block_trackers;
    m->interval = p->block_update == AU_BLOCK_UPDATE_NEVER ? 0 : p->block_update == AU_BLOCK_UPDATE_WEEKLY ? 7 * 86400 : 86400;
    au_blocklist_set_enabled(m->bl, m->on);
    au_blocklist_set_user(m->bl, p->block_allow, p->block_deny, p->block_sites_off);
    /* The lists added by URL: keep the ones still named, add new ones. */
    bool keep[MAX_LISTS];
    memset(keep, 0, sizeof keep);
    int ncustom = 0;
    for (const char *s = p->block_custom ? p->block_custom : ""; *s && ncustom < MAX_CUSTOM;) {
        while (*s == ' ')
            s++;
        size_t n = strcspn(s, " ");
        if (https_url(s, n)) {
            char *url = au_strndup(s, n);
            char id[32];
            snprintf(id, sizeof id, "u-%08lx", (unsigned long)fnv32s(url));
            MList *l = find_list(m, id);
            if (!l && m->nlists < MAX_LISTS) {
                l = &m->lists[m->nlists++];
                memset(l, 0, sizeof *l);
                snprintf(l->id, sizeof l->id, "%s", id);
                /* Until its own title is known: the address without the scheme. */
                snprintf(l->title, sizeof l->title, "%s", url + 8);
                l->url = url;
                url = NULL;
                /* A list seen in an earlier run has its times in the index. */
                if (m->index_dirty)
                    save_index(m);
                m->index_loaded = false;
            }
            if (l)
                keep[l - m->lists] = true;
            au_free(url);
            ncustom++;
        }
        s += n;
    }
    if (!m->index_loaded)
        load_index(m);
    for (size_t i = NCATALOG; i < m->nlists;) {
        if (keep[i]) {
            i++;
            continue;
        }
        MList *l = &m->lists[i];
        if (m->cur == (int)i)
            drop_transfer(m);
        else if (m->cur > (int)i)
            m->cur--;
        unload(m, l);
        if (m->store.remove)
            m->store.remove(m->store.ctx, l->id);
        au_free(l->url);
        memmove(l, l + 1, (m->nlists - i - 1) * sizeof *l);
        memmove(&keep[i], &keep[i + 1], (m->nlists - i - 1) * sizeof keep[0]);
        m->nlists--;
        m->index_dirty = true;
    }
    for (size_t i = 0; i < m->nlists; i++) {
        MList *l = &m->lists[i];
        bool want = m->on && (l->cat ? au_block_words_has(p->block_lists, l->id) : true);
        if (want && !l->enabled) {
            l->enabled = true;
            l->retry_at = 0;
            load_from_store(m, l);
        } else if (!want && l->enabled) {
            l->enabled = false;
            if (m->cur == (int)i)
                drop_transfer(m);
            unload(m, l);
        }
    }
    m->generation++;
}

/* ---- Downloads ---- */

static void fail(AuBlockManager *m, const char *why)
{
    if (m->cur < 0)
        return;
    MList *l = &m->lists[m->cur];
    snprintf(l->error, sizeof l->error, "%s", why);
    l->retry_at = wall(m) + (l->loaded ? RETRY_UPDATE : RETRY_MISSING);
    l->force = false;
    drop_transfer(m);
    m->generation++;
}

static void x_response(void *ctx, AuHttpTransfer *t, const AuHttpResponse *r)
{
    AuBlockManager *m = ctx;
    if (m->cur < 0)
        return;
    MList *l = &m->lists[m->cur];
    if (r->status == 304 && l->loaded) {
        m->got_304 = true;
        return;
    }
    if (r->status != 200 || !r->url || !au_ieq(r->url, strlen(r->url) < 8 ? strlen(r->url) : 8, "https://", 8)) {
        snprintf(m->fail_text, sizeof m->fail_text, r->status == 200 ? "the list did not come over https" : "HTTP %d",
                 r->status);
        m->failed = true;
        au_http_cancel(t);
        m->t = NULL;
        return;
    }
    m->got_200 = true;
    const char *v = au_headers_get(r->headers, "etag");
    clean_field(m->new_etag, sizeof m->new_etag, v ? v : "", v ? strlen(v) : 0);
    v = au_headers_get(r->headers, "last-modified");
    clean_field(m->new_modified, sizeof m->new_modified, v ? v : "", v ? strlen(v) : 0);
    m->builder = au_block_builder_new(mem_of(m));
    if (l->cat)
        au_block_builder_set_title(m->builder, l->cat->title);
    m->generation++;
}

static void x_data(void *ctx, AuHttpTransfer *t, const void *data, size_t len)
{
    AuBlockManager *m = ctx;
    if (!m->builder || m->failed)
        return;
    m->received += (int64_t)len;
    const char *why = NULL;
    if (m->received > (int64_t)MAX_TEXT_BYTES)
        why = "the list is too large";
    else if (!au_block_builder_feed(m->builder, data, len))
        why = "not enough memory for this list";
    if (why) {
        snprintf(m->fail_text, sizeof m->fail_text, "%s", why);
        m->failed = true;
        au_http_cancel(t);
        m->t = NULL;
    }
}

static void x_done(void *ctx, AuHttpTransfer *t, AuNetError err)
{
    AuBlockManager *m = ctx;
    (void)t;
    m->t = NULL;
    if (err != AU_NET_OK && !m->failed) {
        snprintf(m->fail_text, sizeof m->fail_text, "%s", au_net_error_name(err));
        m->failed = true;
    }
}

static bool x_redirect(void *ctx, AuHttpTransfer *t, int status, const char *url)
{
    (void)ctx;
    (void)t;
    (void)status;
    return au_ieq(url, strlen(url) < 8 ? strlen(url) : 8, "https://", 8);
}

static void start(AuBlockManager *m, int i)
{
    MList *l = &m->lists[i];
    m->cur = i;
    m->state = AU_BLOCK_DOWNLOADING;
    m->received = 0;
    m->got_200 = m->got_304 = m->failed = false;
    m->new_etag[0] = m->new_modified[0] = m->fail_text[0] = 0;
    m->t_start = ticks(m);
    m->t_merge = m->merge_calls = 0;
    l->error[0] = 0;
    AuHttpRequest hr;
    au_http_request_init(&hr, list_url(l));
    hr.dest = AU_DEST_EMPTY;
    hr.mode = AU_MODE_NO_CORS;
    hr.omit_credentials = true;
    hr.no_cache = true;
    hr.total_timeout_ms = TIMEOUT_MS;
    if (l->loaded && l->etag[0])
        au_headers_add(&hr.headers, "If-None-Match", l->etag);
    if (l->loaded && l->last_modified[0])
        au_headers_add(&hr.headers, "If-Modified-Since", l->last_modified);
    AuHttpCallbacks cb = {m, x_response, x_data, x_done, x_redirect, NULL};
    m->t = au_http_start(m->net, &hr, &cb);
    au_http_request_free(&hr);
    m->generation++;
}

/* The compiled list is ready: checks it, saves it and puts it in use. */
static void install(AuBlockManager *m)
{
    MList *l = &m->lists[m->cur];
    uint8_t *data = NULL;
    size_t len = 0;
    AuBlockFileInfo info;
    AuBlockBuildStats bs;
    au_block_builder_stats(m->builder, &bs);
    if (!au_block_builder_take(m->builder, &data, &len) || !au_block_file_check(data, len, &info)) {
        release_block(m, data);
        fail(m, "the list could not be compiled");
        return;
    }
    /* Plausibility: an error page, an emptied or truncated list must not
     * replace a working one. */
    uint32_t floor = l->cat ? l->cat->entries / 4 : 1;
    if (l->loaded && l->entries / 4 > floor)
        floor = l->entries / 4;
    if (info.blocks < floor) {
        release_block(m, data);
        fail(m, info.blocks ? "the new list is implausibly small" : "no domains found in the list");
        return;
    }
    bool saved = m->store.write && m->store.write(m->store.ctx, l->id, data, len);
    snprintf(m->last.id, sizeof m->last.id, "%s", l->id);
    m->last.entries = info.blocks;
    m->last.text_bytes = (uint32_t)m->received;
    m->last.file_bytes = (uint32_t)len;
    m->last.merge_ms = m->t_merge;
    m->last.merge_calls = m->merge_calls;
    if (!au_blocklist_attach(m->bl, l->id, data, len, mem_of(m))) {
        fail(m, "the list could not be loaded");
        return;
    }
    l->loaded = true;
    l->tried_store = true;
    l->entries = info.blocks;
    l->bytes = (uint32_t)len;
    if (!l->cat && info.title[0])
        snprintf(l->title, sizeof l->title, "%s", info.title);
    snprintf(l->etag, sizeof l->etag, "%s", m->new_etag);
    snprintf(l->last_modified, sizeof l->last_modified, "%s", m->new_modified);
    l->updated_at = l->checked_at = wall(m);
    l->retry_at = 0;
    l->force = false;
    snprintf(l->error, sizeof l->error, "%s", saved ? "" : "in use, but it could not be saved");
    m->index_dirty = true;
    drop_transfer(m);
    m->generation++;
}

static bool due(AuBlockManager *m, const MList *l, int64_t now)
{
    if (!l->enabled || !m->on)
        return false;
    if (l->force)
        return true;
    if (l->retry_at > now && l->retry_at < now + 2 * RETRY_UPDATE)
        return false;
    if (!l->loaded)
        return true;
    if (!m->interval)
        return false;
    /* A clock that went backwards must not stop updates for good. */
    return now - l->checked_at >= m->interval || l->checked_at > now + 86400;
}

bool au_block_manager_idle(AuBlockManager *m, bool busy)
{
    if (m->cur >= 0) {
        if (m->state == AU_BLOCK_COMPILING) {
            uint32_t t0 = ticks(m);
            int r = au_block_builder_finish(m->builder, MERGE_SLICE);
            m->t_merge += ticks(m) - t0;
            m->merge_calls++;
            if (r < 0)
                fail(m, "not enough memory for this list");
            else if (r > 0)
                install(m);
            return true;
        }
        if (m->t)
            return true; /* the pump delivers */
        MList *l = &m->lists[m->cur];
        if (m->failed) {
            fail(m, m->fail_text[0] ? m->fail_text : "the download failed");
        } else if (m->got_304) {
            l->checked_at = wall(m);
            l->retry_at = 0;
            l->force = false;
            m->index_dirty = true;
            drop_transfer(m);
            m->generation++;
        } else if (m->got_200 && m->builder) {
            m->last.download_ms = ticks(m) - m->t_start;
            m->state = AU_BLOCK_COMPILING;
            m->generation++;
        } else {
            fail(m, "the download failed");
        }
        return true;
    }
    if (busy)
        return false;
    int64_t now = wall(m);
    for (size_t i = 0; i < m->nlists; i++)
        if (due(m, &m->lists[i], now)) {
            start(m, (int)i);
            return true;
        }
    if (m->index_dirty)
        save_index(m);
    return false;
}

void au_block_manager_update_now(AuBlockManager *m)
{
    for (size_t i = 0; i < m->nlists; i++)
        if (m->lists[i].enabled)
            m->lists[i].force = true;
    m->generation++;
}

size_t au_block_manager_count(const AuBlockManager *m) { return m->nlists; }
uint32_t au_block_manager_generation(const AuBlockManager *m) { return m->generation; }
void au_block_manager_compile_stats(const AuBlockManager *m, AuBlockCompileStats *out) { *out = m->last; }

bool au_block_manager_info(const AuBlockManager *m, size_t i, AuBlockListInfo *o)
{
    if (i >= m->nlists)
        return false;
    const MList *l = &m->lists[i];
    memset(o, 0, sizeof *o);
    snprintf(o->id, sizeof o->id, "%s", l->id);
    snprintf(o->title, sizeof o->title, "%s", l->title);
    o->url = list_url(l);
    o->description = l->cat ? l->cat->description : "";
    o->catalog = l->cat != NULL;
    o->security = l->cat && l->cat->security;
    o->enabled = l->enabled;
    o->loaded = l->loaded;
    o->entries = l->loaded ? l->entries : (l->cat ? l->cat->entries : 0);
    o->bytes = l->bytes;
    o->updated_at = l->updated_at;
    o->checked_at = l->checked_at;
    snprintf(o->error, sizeof o->error, "%s", l->error);
    if (m->cur == (int)i) {
        o->state = m->state;
        o->received = m->received;
    } else if (l->enabled && m->on && (!l->loaded || l->force)) {
        o->state = AU_BLOCK_WAITING;
    }
    return true;
}
