/* Web storage: the ordered map, the areas, their policy, limits and
 * files. See web_storage.h. */
#include "web_storage.h"

#include "psl.h"

#include <string.h>

/* ---- The ordered map ----
 * Entries in insertion order, found through an open-addressing table of
 * entry numbers. A removed entry stays as a hole (k is NULL) until the
 * holes outnumber the entries, so removal costs no move. */

typedef struct Entry {
    char *k, *v; /* k is NULL for a hole */
    uint32_t klen, vlen, hash;
} Entry;

#define SLOT_EMPTY 0u
#define SLOT_GONE 0xFFFFFFFFu

struct AuStorageMap {
    Entry *e;
    uint32_t n, cap, live;
    uint32_t *idx; /* entry number plus one, SLOT_EMPTY or SLOT_GONE */
    uint32_t idx_cap, idx_used;
    uint32_t units; /* UTF-16 code units of the live keys and values */
    size_t mem;     /* bytes of the live keys and values */
    uint32_t cur_i, cur_pos; /* key(): entry cur_i is the cur_pos-th live one */
};

uint32_t au_storage_utf16_units(const char *s, size_t n)
{
    uint32_t u = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) != 0x80)
            u += c >= 0xF0 ? 2 : 1;
    }
    return u;
}

static uint32_t hash_bytes(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static char *dup_bytes(const char *s, size_t n)
{
    char *p = au_alloc(n + 1);
    if (n)
        memcpy(p, s, n);
    p[n] = 0;
    return p;
}

AuStorageMap *au_storage_map_new(void)
{
    return au_calloc(sizeof(AuStorageMap));
}

void au_storage_map_clear(AuStorageMap *m)
{
    for (uint32_t i = 0; i < m->n; i++) {
        au_free(m->e[i].k);
        au_free(m->e[i].v);
    }
    au_free(m->e);
    au_free(m->idx);
    memset(m, 0, sizeof *m);
}

void au_storage_map_free(AuStorageMap *m)
{
    if (!m)
        return;
    au_storage_map_clear(m);
    au_free(m);
}

uint32_t au_storage_map_count(const AuStorageMap *m) { return m->live; }
uint32_t au_storage_map_bytes(const AuStorageMap *m) { return m->units * 2; }

/* The slot holding the key, or -1. */
static int32_t find_slot(const AuStorageMap *m, const char *k, size_t klen, uint32_t h)
{
    if (!m->idx_cap)
        return -1;
    uint32_t mask = m->idx_cap - 1;
    for (uint32_t s = h & mask, probes = 0; probes < m->idx_cap; s = (s + 1) & mask, probes++) {
        uint32_t v = m->idx[s];
        if (v == SLOT_EMPTY)
            return -1;
        if (v == SLOT_GONE)
            continue;
        const Entry *e = &m->e[v - 1];
        if (e->hash == h && e->klen == klen && (!klen || !memcmp(e->k, k, klen)))
            return (int32_t)s;
    }
    return -1;
}

static void index_put(AuStorageMap *m, uint32_t entry)
{
    uint32_t mask = m->idx_cap - 1;
    uint32_t s = m->e[entry].hash & mask;
    while (m->idx[s] != SLOT_EMPTY)
        s = (s + 1) & mask;
    m->idx[s] = entry + 1;
    m->idx_used++;
}

/* Drops the holes and builds a table with room for one more entry. */
static void rebuild(AuStorageMap *m)
{
    uint32_t w = 0;
    for (uint32_t i = 0; i < m->n; i++)
        if (m->e[i].k)
            m->e[w++] = m->e[i];
    m->n = w;
    uint32_t want = 16;
    while (want < (m->live + 1) * 2 && want < 0x40000000u)
        want *= 2;
    au_free(m->idx);
    m->idx = au_calloc((size_t)want * sizeof *m->idx);
    m->idx_cap = want;
    m->idx_used = 0;
    for (uint32_t i = 0; i < m->n; i++)
        index_put(m, i);
    m->cur_i = m->cur_pos = 0;
}

bool au_storage_map_get(const AuStorageMap *m, const char *k, size_t klen, const char **v, size_t *vlen)
{
    int32_t s = find_slot(m, k, klen, hash_bytes(k, klen));
    if (s < 0)
        return false;
    const Entry *e = &m->e[m->idx[s] - 1];
    if (v)
        *v = e->v;
    if (vlen)
        *vlen = e->vlen;
    return true;
}

bool au_storage_map_set(AuStorageMap *m, const char *k, size_t klen, const char *v, size_t vlen)
{
    if (klen > 0x7FFFFFFFu || vlen > 0x7FFFFFFFu)
        return false;
    uint32_t h = hash_bytes(k, klen);
    int32_t s = find_slot(m, k, klen, h);
    if (s >= 0) {
        Entry *e = &m->e[m->idx[s] - 1];
        if (e->vlen == vlen && (!vlen || !memcmp(e->v, v, vlen)))
            return false;
        m->units -= au_storage_utf16_units(e->v, e->vlen);
        m->mem -= e->vlen;
        char *nv = dup_bytes(v, vlen);
        au_free(e->v);
        e->v = nv;
        e->vlen = (uint32_t)vlen;
        m->units += au_storage_utf16_units(v, vlen);
        m->mem += vlen;
        return true;
    }
    if (!m->idx_cap || (m->idx_used + 1) * 4 > m->idx_cap * 3)
        rebuild(m);
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8;
        m->e = au_realloc(m->e, (size_t)m->cap * sizeof *m->e);
    }
    Entry *e = &m->e[m->n];
    e->k = dup_bytes(k, klen);
    e->v = dup_bytes(v, vlen);
    e->klen = (uint32_t)klen;
    e->vlen = (uint32_t)vlen;
    e->hash = h;
    index_put(m, m->n);
    m->n++;
    m->live++;
    m->units += au_storage_utf16_units(k, klen) + au_storage_utf16_units(v, vlen);
    m->mem += klen + vlen;
    return true;
}

bool au_storage_map_remove(AuStorageMap *m, const char *k, size_t klen)
{
    int32_t s = find_slot(m, k, klen, hash_bytes(k, klen));
    if (s < 0)
        return false;
    Entry *e = &m->e[m->idx[s] - 1];
    m->units -= au_storage_utf16_units(e->k, e->klen) + au_storage_utf16_units(e->v, e->vlen);
    m->mem -= (size_t)e->klen + e->vlen;
    au_free(e->k);
    au_free(e->v);
    e->k = e->v = NULL;
    m->idx[s] = SLOT_GONE;
    m->live--;
    m->cur_i = m->cur_pos = 0;
    if (!m->live)
        au_storage_map_clear(m);
    else if (m->n - m->live > 16 && m->n - m->live > m->live)
        rebuild(m);
    return true;
}

bool au_storage_map_key(AuStorageMap *m, uint32_t i, const char **k, size_t *klen)
{
    if (i >= m->live)
        return false;
    uint32_t at;
    if (m->n == m->live) {
        at = i;
    } else {
        /* Holes: walk from the last answer when i is at or after it, so
         * that a loop over every key stays linear. */
        uint32_t pos = 0;
        at = 0;
        if (m->cur_pos <= i && m->cur_i < m->n && m->e[m->cur_i].k) {
            at = m->cur_i;
            pos = m->cur_pos;
        }
        for (;; at++) {
            if (!m->e[at].k)
                continue;
            if (pos == i)
                break;
            pos++;
        }
        m->cur_i = at;
        m->cur_pos = i;
    }
    *k = m->e[at].k;
    *klen = m->e[at].klen;
    return true;
}

/* The i-th live entry by position in the array, for serializing. */
static const Entry *next_live(const AuStorageMap *m, uint32_t *at)
{
    while (*at < m->n) {
        const Entry *e = &m->e[(*at)++];
        if (e->k)
            return e;
    }
    return NULL;
}

/* ---- The service ---- */

struct AuStorageArea {
    char *key;
    uint8_t kind;
    uint32_t ns;
    bool session_only; /* local: never written */
    bool loaded;       /* map holds the contents */
    bool dirty;        /* persistent: the file is out of date */
    bool on_disk;      /* a file exists and the index names it */
    char file[9];      /* "" until a file name is chosen */
    AuStorageMap *map;
    uint32_t bytes, items; /* from the map when loaded, else from the index */
    size_t mem;            /* what the area counts for in ws->resident */
    uint32_t stamp;        /* last use */
};

#define ENTRY_COST 48u /* an entry's record, table slot and allocator overhead */

struct AuWebStorage {
    AuStorageArea **areas;
    size_t n, cap;
    AuCookieJar *jar;
    AuWebStorageFiles files;
    bool have_files;
    void (*listener)(void *ctx, const AuStorageChange *c);
    void *listener_ctx;
    uint32_t area_quota, total_quota, resident_max;
    size_t total, resident;
    uint32_t next_ns, stamp, generation, change_gen, session_gen;
    bool index_dirty;
    char (*doomed)[9]; /* files to delete at the next save */
    size_t ndoomed;
    /* The last key worked out, so that a run of calls from one document
     * parses its origin once. */
    struct {
        bool valid;
        char *origin, *top, *key;
        uint32_t jar_gen;
        AuStorageResult result;
        bool session_only;
    } last;
    AuStorageArea *last_area;
    uint32_t reads, writes, damaged;
};

AuWebStorage *au_web_storage_new(void)
{
    AuWebStorage *ws = au_calloc(sizeof *ws);
    ws->area_quota = AU_WEB_STORAGE_AREA_BYTES;
    ws->total_quota = AU_WEB_STORAGE_TOTAL_BYTES;
    ws->resident_max = AU_WEB_STORAGE_RESIDENT_BYTES;
    return ws;
}

static void area_free(AuStorageArea *a)
{
    au_storage_map_free(a->map);
    au_free(a->key);
    au_free(a);
}

static void forget_last(AuWebStorage *ws)
{
    au_free(ws->last.origin);
    au_free(ws->last.top);
    au_free(ws->last.key);
    memset(&ws->last, 0, sizeof ws->last);
}

void au_web_storage_free(AuWebStorage *ws)
{
    if (!ws)
        return;
    for (size_t i = 0; i < ws->n; i++)
        area_free(ws->areas[i]);
    au_free(ws->areas);
    au_free(ws->doomed);
    forget_last(ws);
    au_free(ws);
}

void au_web_storage_set_cookie_jar(AuWebStorage *ws, AuCookieJar *jar)
{
    ws->jar = jar;
    forget_last(ws);
}

void au_web_storage_set_limits(AuWebStorage *ws, uint32_t area_bytes, uint32_t total_bytes, uint32_t resident_bytes)
{
    if (area_bytes)
        ws->area_quota = area_bytes;
    if (total_bytes)
        ws->total_quota = total_bytes;
    if (resident_bytes)
        ws->resident_max = resident_bytes;
}

void au_web_storage_set_listener(AuWebStorage *ws, void (*fn)(void *ctx, const AuStorageChange *c), void *ctx)
{
    ws->listener = fn;
    ws->listener_ctx = ctx;
}

uint32_t au_web_storage_area_quota(const AuWebStorage *ws) { return ws->area_quota; }
uint32_t au_web_storage_generation(const AuWebStorage *ws) { return ws->generation; }
uint32_t au_web_storage_change_gen(const AuWebStorage *ws) { return ws->change_gen; }
uint32_t au_web_storage_session_gen(const AuWebStorage *ws) { return ws->session_gen; }

static bool persistent(const AuStorageArea *a) { return a->kind == AU_STORAGE_LOCAL && !a->session_only; }

/* Brings the area's counts and the service's totals in step with its
 * map after a change. */
static void account(AuWebStorage *ws, AuStorageArea *a)
{
    uint32_t bytes = a->loaded ? au_storage_map_bytes(a->map) : a->bytes;
    uint32_t items = a->loaded ? au_storage_map_count(a->map) : a->items;
    size_t mem = a->loaded ? a->map->mem + (size_t)items * ENTRY_COST : 0;
    ws->total = ws->total - a->bytes + bytes;
    ws->resident = ws->resident - a->mem + mem;
    a->bytes = bytes;
    a->items = items;
    a->mem = mem;
}

static void changed(AuWebStorage *ws, AuStorageArea *a)
{
    account(ws, a);
    ws->generation++;
    if (a->kind == AU_STORAGE_SESSION) {
        ws->session_gen++;
    } else if (!a->session_only) {
        a->dirty = true;
        ws->change_gen++;
    }
}

/* ---- Files ---- */

static bool hex_name(const char *s)
{
    for (int i = 0; i < 8; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    return s[8] == 0;
}

static bool name_taken(const AuWebStorage *ws, const char *name)
{
    for (size_t i = 0; i < ws->n; i++)
        if (!strcmp(ws->areas[i]->file, name))
            return true;
    return false;
}

static void undoom(AuWebStorage *ws, const char *name)
{
    for (size_t i = 0; i < ws->ndoomed; i++)
        if (!strcmp(ws->doomed[i], name)) {
            memmove(ws->doomed + i, ws->doomed + i + 1, (ws->ndoomed - i - 1) * sizeof *ws->doomed);
            ws->ndoomed--;
            return;
        }
}

static void doom(AuWebStorage *ws, const char *name)
{
    if (!name[0])
        return;
    undoom(ws, name);
    ws->doomed = au_realloc(ws->doomed, (ws->ndoomed + 1) * sizeof *ws->doomed);
    memcpy(ws->doomed[ws->ndoomed++], name, 9);
    ws->index_dirty = true;
}

static void choose_file(AuWebStorage *ws, AuStorageArea *a)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t h = hash_bytes(a->key, strlen(a->key));
    char name[9];
    for (;; h++) {
        for (int i = 0; i < 8; i++)
            name[i] = hex[(h >> (28 - 4 * i)) & 15];
        name[8] = 0;
        if (!name_taken(ws, name))
            break;
    }
    memcpy(a->file, name, 9);
    undoom(ws, name); /* the write replaces a file that was to go */
}

static uint32_t rd32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void wr32(AuBuf *b, uint32_t v)
{
    char c[4] = {(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v};
    au_buf_append(b, c, 4);
}

bool au_web_storage_parse_file(const char *data, size_t len, const char *key, AuStorageMap *m)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t klen = strlen(key);
    if (len < 20 || len > AU_WEB_STORAGE_FILE_MAX || memcmp(p, "AuLS", 4) != 0 || rd32(p + 4) != 1)
        return false;
    if (rd32(p + len - 4) != hash_bytes(data, len - 4))
        return false;
    size_t end = len - 4, at = 8;
    uint32_t n = rd32(p + at);
    at += 4;
    if (n != klen || n > end - at || memcmp(p + at, key, klen) != 0)
        return false;
    at += n;
    if (end - at < 4)
        return false;
    uint32_t count = rd32(p + at);
    at += 4;
    if (count > (end - at) / 8)
        return false;
    for (uint32_t i = 0; i < count; i++) {
        if (end - at < 4)
            goto bad;
        uint32_t kl = rd32(p + at);
        at += 4;
        if (kl > end - at)
            goto bad;
        const char *k = data + at;
        at += kl;
        if (end - at < 4)
            goto bad;
        uint32_t vl = rd32(p + at);
        at += 4;
        if (vl > end - at)
            goto bad;
        au_storage_map_set(m, k, kl, data + at, vl);
        at += vl;
    }
    if (at != end)
        goto bad;
    return true;
bad:
    au_storage_map_clear(m);
    return false;
}

static void serialize_area(const AuStorageArea *a, AuBuf *out)
{
    au_buf_append(out, "AuLS", 4);
    wr32(out, 1);
    size_t klen = strlen(a->key);
    wr32(out, (uint32_t)klen);
    au_buf_append(out, a->key, klen);
    wr32(out, au_storage_map_count(a->map));
    uint32_t at = 0;
    const Entry *e;
    au_buf_reserve(out, a->map->mem + (size_t)a->map->live * 8 + 4);
    while ((e = next_live(a->map, &at)) != NULL) {
        wr32(out, e->klen);
        au_buf_append(out, e->k, e->klen);
        wr32(out, e->vlen);
        au_buf_append(out, e->v, e->vlen);
    }
    wr32(out, hash_bytes(out->data, out->len));
}

static void write_index(AuWebStorage *ws)
{
    AuBuf b;
    au_buf_init(&b);
    au_buf_puts(&b, "# Aura local storage 1\n");
    for (size_t i = 0; i < ws->n; i++) {
        const AuStorageArea *a = ws->areas[i];
        if (a->on_disk)
            au_buf_printf(&b, "%s\t%lu\t%lu\t%s\n", a->file, (unsigned long)a->bytes, (unsigned long)a->items, a->key);
    }
    if (ws->files.write(ws->files.ctx, "Index", b.data ? b.data : "", b.len)) {
        ws->index_dirty = false;
        ws->writes++;
    }
    au_buf_free(&b);
}

static bool key_text_ok(const char *s, size_t n)
{
    if (!n || n > 2048)
        return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || s[i] == 0x7F)
            return false;
    return true;
}

static AuStorageArea *find_area(AuWebStorage *ws, int kind, uint32_t ns, const char *key)
{
    AuStorageArea *l = ws->last_area;
    if (l && l->kind == kind && l->ns == ns && !strcmp(l->key, key))
        return l;
    for (size_t i = 0; i < ws->n; i++) {
        AuStorageArea *a = ws->areas[i];
        if (a->kind == kind && a->ns == ns && !strcmp(a->key, key)) {
            ws->last_area = a;
            return a;
        }
    }
    return NULL;
}

static AuStorageArea *add_area(AuWebStorage *ws, int kind, uint32_t ns, const char *key)
{
    AuStorageArea *a = au_calloc(sizeof *a);
    a->key = au_strndup(key, strlen(key));
    a->kind = (uint8_t)kind;
    a->ns = ns;
    a->stamp = ++ws->stamp;
    if (ws->n == ws->cap) {
        ws->cap = ws->cap ? ws->cap * 2 : 16;
        ws->areas = au_realloc(ws->areas, ws->cap * sizeof *ws->areas);
    }
    ws->areas[ws->n++] = a;
    ws->generation++;
    return a;
}

static void remove_index(AuWebStorage *ws, size_t i)
{
    AuStorageArea *a = ws->areas[i];
    if (a->on_disk)
        doom(ws, a->file);
    if (a->kind == AU_STORAGE_SESSION)
        ws->session_gen++;
    else if (!a->session_only)
        ws->change_gen++;
    ws->total -= a->bytes;
    ws->resident -= a->mem;
    if (ws->last_area == a)
        ws->last_area = NULL;
    area_free(a);
    memmove(ws->areas + i, ws->areas + i + 1, (ws->n - i - 1) * sizeof *ws->areas);
    ws->n--;
    ws->generation++;
}

static void remove_area(AuWebStorage *ws, AuStorageArea *a)
{
    for (size_t i = 0; i < ws->n; i++)
        if (ws->areas[i] == a) {
            remove_index(ws, i);
            return;
        }
}

void au_web_storage_set_files(AuWebStorage *ws, const AuWebStorageFiles *files)
{
    ws->files = *files;
    ws->have_files = true;
    AuBuf b;
    au_buf_init(&b);
    if (!files->read(files->ctx, "Index", &b)) {
        au_buf_free(&b);
        return;
    }
    ws->reads++;
    bool ok = b.len >= 22 && !strncmp(b.data, "# Aura local storage 1", 22);
    size_t i = 0;
    while (ok && i < b.len) {
        size_t e = i;
        while (e < b.len && b.data[e] != '\n')
            e++;
        const char *l = b.data + i;
        size_t n = e - i;
        i = e < b.len ? e + 1 : e;
        if (!n || l[0] == '#')
            continue;
        /* file \t bytes \t items \t key */
        char name[9];
        if (n < 9 || l[8] != '\t') {
            ok = false;
            break;
        }
        memcpy(name, l, 8);
        name[8] = 0;
        size_t at = 9;
        uint32_t num[2] = {0, 0};
        bool line_ok = hex_name(name);
        for (int f = 0; f < 2 && line_ok; f++) {
            size_t digits = 0;
            uint32_t v = 0;
            while (at < n && l[at] >= '0' && l[at] <= '9' && digits < 9) {
                v = v * 10 + (uint32_t)(l[at] - '0');
                at++;
                digits++;
            }
            if (!digits || at >= n || l[at] != '\t')
                line_ok = false;
            at++;
            num[f] = v;
        }
        if (!line_ok || at > n || !key_text_ok(l + at, n - at)) {
            ok = false;
            continue;
        }
        char *key = au_strndup(l + at, n - at);
        if (ws->n < AU_WEB_STORAGE_MAX_AREAS && !name_taken(ws, name) && !find_area(ws, AU_STORAGE_LOCAL, 0, key)) {
            AuStorageArea *a = add_area(ws, AU_STORAGE_LOCAL, 0, key);
            memcpy(a->file, name, 9);
            a->on_disk = true;
            a->bytes = num[0];
            a->items = num[1];
            ws->total += a->bytes;
        }
        au_free(key);
    }
    au_buf_free(&b);
    if (!ok) {
        /* What was read is kept; the rest of the file is set aside. */
        ws->damaged++;
        if (files->damaged)
            files->damaged(files->ctx, "Index");
        ws->index_dirty = true;
    }
}

static void unload(AuWebStorage *ws, AuStorageArea *a)
{
    au_storage_map_free(a->map);
    a->map = NULL;
    a->loaded = false;
    account(ws, a);
}

static bool write_area(AuWebStorage *ws, AuStorageArea *a)
{
    if (!a->file[0])
        choose_file(ws, a);
    AuBuf b;
    au_buf_init(&b);
    serialize_area(a, &b);
    bool ok = ws->files.write(ws->files.ctx, a->file, b.data, b.len);
    au_buf_free(&b);
    if (ok) {
        a->dirty = false;
        a->on_disk = true;
        ws->index_dirty = true;
        ws->writes++;
    }
    return ok;
}

/* The least recently used area that pick accepts, other than keep. */
static AuStorageArea *lru(AuWebStorage *ws, const AuStorageArea *keep, bool (*pick)(const AuStorageArea *))
{
    AuStorageArea *best = NULL;
    for (size_t i = 0; i < ws->n; i++) {
        AuStorageArea *a = ws->areas[i];
        if (a != keep && pick(a) && (!best || (int32_t)(a->stamp - best->stamp) < 0))
            best = a;
    }
    return best;
}

static bool pick_session_only(const AuStorageArea *a) { return a->kind == AU_STORAGE_LOCAL && a->session_only; }
static bool pick_clean(const AuStorageArea *a) { return persistent(a) && a->loaded && !a->dirty && a->on_disk; }
static bool pick_dirty(const AuStorageArea *a) { return persistent(a) && a->loaded && a->dirty; }

/* Makes room for bytes more in the total and mem more in memory, never
 * touching keep. False when it cannot. */
static bool make_room(AuWebStorage *ws, const AuStorageArea *keep, size_t bytes, size_t mem)
{
    AuStorageArea *a;
    while (ws->total + bytes > ws->total_quota) {
        if (!(a = lru(ws, keep, pick_session_only)))
            return false;
        remove_area(ws, a);
    }
    while (ws->resident + mem > ws->resident_max) {
        if ((a = lru(ws, keep, pick_clean)) != NULL) {
            unload(ws, a);
        } else if (ws->have_files && (a = lru(ws, keep, pick_dirty)) != NULL && write_area(ws, a)) {
            unload(ws, a);
        } else if ((a = lru(ws, keep, pick_session_only)) != NULL) {
            remove_area(ws, a);
        } else {
            /* Only tabs' session areas, areas that cannot be written and
             * keep itself are left: one area may always be in memory. */
            return ws->resident - (keep ? keep->mem : 0) == 0;
        }
    }
    return true;
}

static void ensure_loaded(AuWebStorage *ws, AuStorageArea *a)
{
    if (a->loaded)
        return;
    a->map = au_storage_map_new();
    a->loaded = true;
    if (a->on_disk && ws->have_files) {
        AuBuf b;
        au_buf_init(&b);
        bool ok = ws->files.read(ws->files.ctx, a->file, &b);
        ws->reads++;
        if (ok)
            ok = au_web_storage_parse_file(b.data ? b.data : "", b.len, a->key, a->map);
        au_buf_free(&b);
        if (!ok) {
            ws->damaged++;
            if (ws->files.damaged)
                ws->files.damaged(ws->files.ctx, a->file);
            else
                ws->files.remove(ws->files.ctx, a->file);
            a->on_disk = false;
            a->dirty = false;
            ws->index_dirty = true;
            ws->generation++;
        }
    }
    account(ws, a);
    make_room(ws, a, 0, 0);
}

uint32_t au_web_storage_pending(const AuWebStorage *ws)
{
    uint32_t n = (uint32_t)ws->ndoomed + (ws->index_dirty ? 1 : 0);
    for (size_t i = 0; i < ws->n; i++)
        if (persistent(ws->areas[i]) && ws->areas[i]->dirty)
            n++;
    return n;
}

uint32_t au_web_storage_save(AuWebStorage *ws, uint32_t max)
{
    if (!ws->have_files)
        return 0;
    uint32_t done = 0;
    bool more = false;
    for (size_t i = 0; i < ws->n; i++) {
        AuStorageArea *a = ws->areas[i];
        if (!persistent(a) || !a->dirty)
            continue;
        if (max && done >= max) {
            more = true;
            break;
        }
        if (write_area(ws, a))
            done++;
    }
    for (size_t i = 0; i < ws->ndoomed; i++) {
        ws->files.remove(ws->files.ctx, ws->doomed[i]);
        done++;
    }
    ws->ndoomed = 0;
    if (ws->index_dirty && !more) {
        write_index(ws);
        done++;
    }
    return done;
}

size_t au_web_storage_release(AuWebStorage *ws)
{
    size_t before = ws->resident;
    for (size_t i = 0; i < ws->n; i++)
        if (pick_clean(ws->areas[i]))
            unload(ws, ws->areas[i]);
    return before - ws->resident;
}

/* ---- Keys and policy ---- */

static AuStorageResult work_out_key(AuWebStorage *ws, const char *origin, const char *top_url, AuBuf *key,
                                    bool *session_only)
{
    *session_only = false;
    au_buf_clear(key);
    if (!origin || !key_text_ok(origin, strlen(origin)) || !strcmp(origin, "null") || strchr(origin, '^'))
        return AU_STORAGE_DENIED;
    AuUrl o;
    if (!au_url_parse(&o, origin, NULL))
        return AU_STORAGE_DENIED;
    bool is_file = !strcmp(o.scheme, "file");
    if (!is_file && !o.host[0]) {
        au_url_free(&o);
        return AU_STORAGE_DENIED;
    }
    AuCookieRule rule = AU_COOKIE_RULE_NONE;
    AuNetCookiePolicy policy = AU_COOKIE_POLICY_ALL;
    bool block_third = false;
    if (ws->jar) {
        rule = au_cookie_jar_rule(ws->jar, o.host);
        policy = au_cookie_jar_policy(ws->jar, &block_third);
    }
    if (rule == AU_COOKIE_RULE_BLOCK || (policy == AU_COOKIE_POLICY_NONE && rule != AU_COOKIE_RULE_ALLOW)) {
        au_url_free(&o);
        return AU_STORAGE_DENIED;
    }
    au_buf_puts(key, is_file ? "file://" : origin);
    if (top_url) {
        AuUrl t;
        bool parsed = au_url_parse(&t, top_url, NULL);
        bool third = !parsed || (is_file ? strcmp(t.scheme, "file") != 0 : !au_same_registrable(t.host, o.host));
        if (third) {
            AuBuf site;
            au_buf_init(&site);
            if (parsed)
                au_url_site(&t, &site);
            au_buf_putc(key, '^');
            if (site.len && key_text_ok(site.data, site.len) && !memchr(site.data, '^', site.len))
                au_buf_append(key, site.data, site.len);
            else
                au_buf_puts(key, "null");
            au_buf_free(&site);
            if (rule != AU_COOKIE_RULE_ALLOW && (block_third || policy == AU_COOKIE_POLICY_SAME_SITE))
                *session_only = true;
        }
        if (parsed)
            au_url_free(&t);
    }
    if (rule == AU_COOKIE_RULE_SESSION)
        *session_only = true;
    au_url_free(&o);
    au_buf_cstr(key);
    return AU_STORAGE_OK;
}

static bool same_str(const char *a, const char *b)
{
    return a == b || (a && b && !strcmp(a, b));
}

/* The key through the one-entry memory. */
static AuStorageResult resolve(AuWebStorage *ws, const char *origin, const char *top_url, const char **key,
                               bool *session_only)
{
    uint32_t gen = ws->jar ? au_cookie_jar_policy_gen(ws->jar) : 0;
    if (!ws->last.valid || ws->last.jar_gen != gen || !same_str(ws->last.origin, origin) ||
        !same_str(ws->last.top, top_url)) {
        AuBuf k;
        au_buf_init(&k);
        bool so;
        AuStorageResult r = work_out_key(ws, origin, top_url, &k, &so);
        forget_last(ws);
        ws->last.valid = true;
        ws->last.origin = origin ? au_strndup(origin, strlen(origin)) : NULL;
        ws->last.top = top_url ? au_strndup(top_url, strlen(top_url)) : NULL;
        ws->last.key = au_buf_take(&k, NULL);
        ws->last.jar_gen = gen;
        ws->last.result = r;
        ws->last.session_only = so;
    }
    *key = ws->last.key;
    *session_only = ws->last.session_only;
    return ws->last.result;
}

AuStorageResult au_web_storage_key(AuWebStorage *ws, const char *origin, const char *top_url, AuBuf *key,
                                   bool *session_only)
{
    const char *k;
    bool so;
    AuStorageResult r = resolve(ws, origin, top_url, &k, &so);
    au_buf_clear(key);
    if (r == AU_STORAGE_OK)
        au_buf_puts(key, k);
    au_buf_cstr(key);
    if (session_only)
        *session_only = so;
    return r;
}

AuStorageResult au_web_storage_open(AuWebStorage *ws, int kind, uint32_t ns, const char *origin, const char *top_url,
                                    bool create, AuStorageArea **area)
{
    *area = NULL;
    const char *key;
    bool so;
    AuStorageResult r = resolve(ws, origin, top_url, &key, &so);
    if (r != AU_STORAGE_OK)
        return r;
    if (kind != AU_STORAGE_SESSION) {
        kind = AU_STORAGE_LOCAL;
        ns = 0;
    }
    AuStorageArea *a = find_area(ws, kind, ns, key);
    if (!a) {
        if (!create)
            return AU_STORAGE_OK;
        if (ws->n >= AU_WEB_STORAGE_MAX_AREAS) {
            AuStorageArea *old = lru(ws, NULL, pick_session_only);
            if (!old)
                return AU_STORAGE_QUOTA;
            remove_area(ws, old);
        }
        a = add_area(ws, kind, ns, key);
        a->session_only = kind == AU_STORAGE_LOCAL && so;
        a->map = au_storage_map_new();
        a->loaded = true;
    }
    a->stamp = ++ws->stamp;
    ensure_loaded(ws, a);
    if (kind == AU_STORAGE_LOCAL && a->session_only != so) {
        /* The site's rule or the preference changed. */
        if (so) {
            if (a->on_disk)
                doom(ws, a->file);
            a->on_disk = false;
            a->dirty = false;
            a->file[0] = 0;
        } else {
            a->dirty = true;
        }
        a->session_only = so;
        ws->change_gen++;
        ws->generation++;
    }
    if (!create && !a->items) {
        /* Nothing in it (a damaged file, say): no record is kept. */
        remove_area(ws, a);
        return AU_STORAGE_OK;
    }
    ws->last_area = a;
    *area = a;
    return AU_STORAGE_OK;
}

AuStorageMap *au_storage_area_map(AuStorageArea *a) { return a->map; }
const char *au_storage_area_key(const AuStorageArea *a) { return a->key; }

static void tell(AuWebStorage *ws, const AuStorageArea *a, const char *k, size_t klen, const char *ov, size_t olen,
                 const char *nv, size_t nlen, const char *url, const void *writer)
{
    if (!ws->listener)
        return;
    AuStorageChange c;
    memset(&c, 0, sizeof c);
    c.kind = a->kind;
    c.ns = a->ns;
    c.storage_key = a->key;
    c.key = k;
    c.key_len = klen;
    c.old_value = ov;
    c.old_len = olen;
    c.new_value = nv;
    c.new_len = nlen;
    c.url = url;
    c.writer = writer;
    ws->listener(ws->listener_ctx, &c);
}

AuStorageResult au_web_storage_set(AuWebStorage *ws, AuStorageArea *a, const char *k, size_t klen, const char *v,
                                   size_t vlen, const char *url, const void *writer)
{
    const char *ov = NULL;
    size_t olen = 0;
    bool had = au_storage_map_get(a->map, k, klen, &ov, &olen);
    if (had && olen == vlen && (!vlen || !memcmp(ov, v, vlen)))
        return AU_STORAGE_OK;
    /* Sizes in 64 bits: a string may be up to 2 GB. */
    uint64_t now = a->bytes;
    uint64_t gone = had ? 2ull * au_storage_utf16_units(ov, olen) : 0;
    uint64_t added = 2ull * au_storage_utf16_units(v, vlen) + (had ? 0 : 2ull * au_storage_utf16_units(k, klen));
    if (now - gone + added > ws->area_quota) {
        if (a->items == 0)
            remove_area(ws, a);
        return AU_STORAGE_QUOTA;
    }
    size_t grow_bytes = added > gone ? (size_t)(added - gone) : 0;
    size_t grow_mem = had ? (vlen > olen ? vlen - olen : 0) : klen + vlen + ENTRY_COST;
    if (!make_room(ws, a, grow_bytes, grow_mem)) {
        if (a->items == 0)
            remove_area(ws, a);
        return AU_STORAGE_QUOTA;
    }
    tell(ws, a, k, klen, ov, olen, v, vlen, url, writer);
    au_storage_map_set(a->map, k, klen, v, vlen);
    changed(ws, a);
    return AU_STORAGE_OK;
}

void au_web_storage_remove(AuWebStorage *ws, AuStorageArea *a, const char *k, size_t klen, const char *url,
                           const void *writer)
{
    const char *ov = NULL;
    size_t olen = 0;
    if (!au_storage_map_get(a->map, k, klen, &ov, &olen))
        return;
    tell(ws, a, k, klen, ov, olen, NULL, 0, url, writer);
    au_storage_map_remove(a->map, k, klen);
    changed(ws, a);
    if (!a->items)
        remove_area(ws, a);
}

void au_web_storage_clear_area(AuWebStorage *ws, AuStorageArea *a, const char *url, const void *writer)
{
    if (a->items)
        tell(ws, a, NULL, 0, NULL, 0, NULL, 0, url, writer);
    remove_area(ws, a);
}

int au_web_storage_call(AuWebStorage *ws, uint32_t ns, AuJsStorageCall *c)
{
    int kind = c->kind == AU_JS_STORAGE_SESSION ? AU_STORAGE_SESSION : AU_STORAGE_LOCAL;
    if (c->op == AU_JS_STORAGE_MATCH) {
        const char *key;
        bool so;
        bool same = resolve(ws, c->origin, c->top_url, &key, &so) == AU_STORAGE_OK && strlen(key) == c->key_len &&
                    !memcmp(key, c->key, c->key_len);
        return same ? AU_JS_STORAGE_OK : AU_JS_STORAGE_MISSING;
    }
    AuStorageArea *a = NULL;
    AuStorageResult r = au_web_storage_open(ws, kind, ns, c->origin, c->top_url, c->op == AU_JS_STORAGE_SET, &a);
    if (r != AU_STORAGE_OK)
        return r == AU_STORAGE_QUOTA ? AU_JS_STORAGE_QUOTA : AU_JS_STORAGE_DENIED;
    AuStorageMap *m = a ? a->map : NULL;
    switch (c->op) {
    case AU_JS_STORAGE_LENGTH:
        c->length = m ? au_storage_map_count(m) : 0;
        break;
    case AU_JS_STORAGE_KEY:
        if (!m || !au_storage_map_key(m, c->index, &c->out, &c->out_len))
            return AU_JS_STORAGE_MISSING;
        break;
    case AU_JS_STORAGE_GET:
        if (!m || !au_storage_map_get(m, c->key, c->key_len, &c->out, &c->out_len))
            return AU_JS_STORAGE_MISSING;
        break;
    case AU_JS_STORAGE_SET:
        if (au_web_storage_set(ws, a, c->key, c->key_len, c->value, c->value_len, c->url, c->writer) != AU_STORAGE_OK)
            return AU_JS_STORAGE_QUOTA;
        break;
    case AU_JS_STORAGE_REMOVE:
        if (a)
            au_web_storage_remove(ws, a, c->key, c->key_len, c->url, c->writer);
        break;
    case AU_JS_STORAGE_CLEAR:
        if (a)
            au_web_storage_clear_area(ws, a, c->url, c->writer);
        break;
    case AU_JS_STORAGE_ESTIMATE:
        c->usage = m ? au_storage_map_bytes(m) : 0;
        c->quota = ws->area_quota;
        break;
    default:
        break;
    }
    return AU_JS_STORAGE_OK;
}

/* ---- Session namespaces ---- */

uint32_t au_web_storage_ns_new(AuWebStorage *ws)
{
    if (++ws->next_ns == 0)
        ws->next_ns = 1;
    return ws->next_ns;
}

void au_web_storage_ns_free(AuWebStorage *ws, uint32_t ns)
{
    for (size_t i = ws->n; i-- > 0;)
        if (ws->areas[i]->kind == AU_STORAGE_SESSION && ws->areas[i]->ns == ns)
            remove_index(ws, i);
}

void au_web_storage_ns_copy(AuWebStorage *ws, uint32_t from, uint32_t to)
{
    if (from == to)
        return;
    au_web_storage_ns_free(ws, to);
    size_t n = ws->n; /* the copies go after */
    for (size_t i = 0; i < n; i++) {
        AuStorageArea *s = ws->areas[i];
        if (s->kind != AU_STORAGE_SESSION || s->ns != from || !s->items)
            continue;
        if (ws->n >= AU_WEB_STORAGE_MAX_AREAS || !make_room(ws, s, s->bytes, s->mem))
            break;
        AuStorageArea *d = add_area(ws, AU_STORAGE_SESSION, to, s->key);
        d->map = au_storage_map_new();
        d->loaded = true;
        uint32_t at = 0;
        const Entry *e;
        while ((e = next_live(s->map, &at)) != NULL)
            au_storage_map_set(d->map, e->k, e->klen, e->v, e->vlen);
        changed(ws, d);
    }
}

static void put_escaped(AuBuf *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '%': au_buf_puts(out, "%25"); break;
        case '\t': au_buf_puts(out, "%09"); break;
        case '\r': au_buf_puts(out, "%0D"); break;
        case '\n': au_buf_puts(out, "%0A"); break;
        case 0: au_buf_puts(out, "%00"); break;
        default: au_buf_putc(out, s[i]);
        }
    }
}

/* Whether what the site of a storage key stores may go to disk: not when
 * its cookies may not (cookies off, a "session only" or "block" rule, a
 * third-party area while third-party cookies are blocked). */
static bool key_may_persist(const AuWebStorage *ws, const char *key)
{
    if (!ws->jar)
        return true;
    const char *cut = strchr(key, '^');
    AuUrl o;
    if (!au_url_parse_n(&o, key, cut ? (size_t)(cut - key) : strlen(key), NULL))
        return false;
    bool block_third = false;
    AuNetCookiePolicy policy = au_cookie_jar_policy(ws->jar, &block_third);
    AuCookieRule rule = au_cookie_jar_rule(ws->jar, o.host);
    au_url_free(&o);
    if (rule == AU_COOKIE_RULE_ALLOW)
        return true;
    if (rule != AU_COOKIE_RULE_NONE || policy == AU_COOKIE_POLICY_NONE)
        return false;
    return !(cut && (block_third || policy == AU_COOKIE_POLICY_SAME_SITE));
}

void au_web_storage_ns_serialize(const AuWebStorage *ws, uint32_t ns, AuBuf *out)
{
    size_t left = AU_WEB_STORAGE_SESSION_SAVE_BYTES;
    for (size_t i = 0; i < ws->n; i++) {
        const AuStorageArea *a = ws->areas[i];
        if (a->kind != AU_STORAGE_SESSION || a->ns != ns || !a->items || !key_may_persist(ws, a->key))
            continue;
        size_t start = out->len;
        au_buf_puts(out, "storage\t");
        put_escaped(out, a->key, strlen(a->key));
        uint32_t at = 0;
        const Entry *e;
        while ((e = next_live(a->map, &at)) != NULL && out->len - start <= left) {
            au_buf_putc(out, '\t');
            put_escaped(out, e->k, e->klen);
            au_buf_putc(out, '\t');
            put_escaped(out, e->v, e->vlen);
        }
        au_buf_putc(out, '\n');
        if (out->len - start > left)
            out->len = start; /* too large: left out */
        else
            left -= out->len - start;
    }
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/* Appends the field at s (up to a tab or the end) unescaped; returns its
 * escaped length, or (size_t)-1 for a bad escape. */
static size_t take_field(const char *s, size_t n, AuBuf *out)
{
    size_t i = 0;
    au_buf_clear(out);
    while (i < n && s[i] != '\t') {
        if (s[i] == '%') {
            if (n - i < 3 || hex_val(s[i + 1]) < 0 || hex_val(s[i + 2]) < 0)
                return (size_t)-1;
            au_buf_putc(out, (char)(hex_val(s[i + 1]) * 16 + hex_val(s[i + 2])));
            i += 3;
        } else {
            au_buf_putc(out, s[i++]);
        }
    }
    return i;
}

bool au_web_storage_ns_restore(AuWebStorage *ws, uint32_t ns, const char *line, size_t len)
{
    AuBuf key, k, v;
    au_buf_init(&key);
    au_buf_init(&k);
    au_buf_init(&v);
    AuStorageMap *m = au_storage_map_new();
    bool ok = false;
    size_t at = take_field(line, len, &key);
    if (at == (size_t)-1 || !key_text_ok(key.data, key.len) || len > AU_WEB_STORAGE_SESSION_SAVE_BYTES * 3u)
        goto done;
    while (at < len) {
        at++; /* the tab */
        size_t n = take_field(line + at, len - at, &k);
        if (n == (size_t)-1 || at + n >= len)
            goto done; /* a key without a value */
        at += n + 1;
        n = take_field(line + at, len - at, &v);
        if (n == (size_t)-1)
            goto done;
        at += n;
        au_storage_map_set(m, k.data ? k.data : "", k.len, v.data ? v.data : "", v.len);
    }
    if (!au_storage_map_count(m) || au_storage_map_bytes(m) > ws->area_quota)
        goto done;
    {
        AuStorageArea *old = find_area(ws, AU_STORAGE_SESSION, ns, au_buf_cstr(&key));
        if (old)
            remove_area(ws, old);
        if (ws->n >= AU_WEB_STORAGE_MAX_AREAS ||
            !make_room(ws, NULL, au_storage_map_bytes(m), m->mem + (size_t)m->live * ENTRY_COST))
            goto done;
        AuStorageArea *a = add_area(ws, AU_STORAGE_SESSION, ns, au_buf_cstr(&key));
        a->map = m;
        a->loaded = true;
        m = NULL;
        account(ws, a);
        ok = true;
    }
done:
    au_storage_map_free(m);
    au_buf_free(&key);
    au_buf_free(&k);
    au_buf_free(&v);
    return ok;
}

/* ---- The manager's view ---- */

size_t au_web_storage_count(const AuWebStorage *ws) { return ws->n; }

bool au_web_storage_info(const AuWebStorage *ws, size_t i, AuStorageAreaInfo *out)
{
    if (i >= ws->n)
        return false;
    const AuStorageArea *a = ws->areas[i];
    out->key = a->key;
    out->kind = a->kind;
    out->ns = a->ns;
    out->bytes = a->bytes;
    out->items = a->items;
    out->session_only = a->kind == AU_STORAGE_LOCAL && a->session_only;
    return true;
}

void au_web_storage_site(const char *storage_key, AuBuf *out)
{
    au_buf_clear(out);
    const char *cut = strchr(storage_key, '^');
    size_t n = cut ? (size_t)(cut - storage_key) : strlen(storage_key);
    AuUrl u;
    if (!au_url_parse_n(&u, storage_key, n, NULL)) {
        au_buf_cstr(out);
        return;
    }
    if (!strcmp(u.scheme, "file"))
        au_buf_puts(out, "local files");
    else
        au_cookie_site_key(u.host, out);
    au_url_free(&u);
    au_buf_cstr(out);
}

void au_web_storage_remove_at(AuWebStorage *ws, size_t i)
{
    if (i < ws->n)
        remove_index(ws, i);
}

size_t au_web_storage_remove_site(AuWebStorage *ws, const char *site)
{
    size_t removed = 0;
    AuBuf s;
    au_buf_init(&s);
    for (size_t i = ws->n; i-- > 0;) {
        au_web_storage_site(ws->areas[i]->key, &s);
        if (!strcmp(au_buf_cstr(&s), site)) {
            remove_index(ws, i);
            removed++;
        }
    }
    au_buf_free(&s);
    return removed;
}

void au_web_storage_clear(AuWebStorage *ws)
{
    while (ws->n)
        remove_index(ws, ws->n - 1);
}

void au_web_storage_stats(const AuWebStorage *ws, AuWebStorageStats *out)
{
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < ws->n; i++) {
        const AuStorageArea *a = ws->areas[i];
        if (a->kind == AU_STORAGE_SESSION) {
            out->session_areas++;
        } else {
            if (a->session_only)
                out->session_only_areas++;
            else
                out->local_areas++;
            if (a->loaded)
                out->loaded++;
        }
    }
    out->bytes = ws->total;
    out->resident = ws->resident;
    out->pending = au_web_storage_pending(ws);
    out->reads = ws->reads;
    out->writes = ws->writes;
    out->damaged = ws->damaged;
}
