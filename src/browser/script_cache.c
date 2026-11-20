/* The script cache (script_cache.h). */
#include "script_cache.h"
#include "../js/hint_store.h"
#include "../net/disk_cache.h"

#include <string.h>

#define KEY AU_JS_CODE_KEY
#define NAME_BYTES 15 /* of the key, in a file's name: 30 characters */
#define SEEN_BYTES 16 /* of the key, in the Index */

/* A script's file: MAGIC, the key, the bytecode's length and checksum
 * (big-endian), the bytecode. */
static const char MAGIC[8] = {'A', 'u', 'S', 'C', '1', 0, 0, 0};
#define FILE_HEAD (sizeof MAGIC + KEY + 8)

typedef struct Entry {
    uint8_t key[KEY];
    size_t size;   /* of the file */
    uint32_t age;  /* orders the entries; the largest is the newest */
    uint8_t *file; /* the file's bytes: queued, or kept in memory (no files) */
    bool known;    /* key is whole; false for a file found at start, whose name gives NAME_BYTES of it */
} Entry;

struct AuScriptCache {
    AuDiskCacheFiles files;
    bool has_files, deferred, dirty;
    size_t max, bytes, pending;
    Entry *e;
    size_t n, cap;
    uint32_t clock;
    uint8_t seen[AU_SCRIPT_CACHE_SEEN][SEEN_BYTES]; /* the most recent first */
    size_t nseen;
    AuScriptCacheStats st;
    AuJsCodeStore store;
};

static uint32_t checksum(const uint8_t *p, size_t len)
{
    uint32_t a = 2166136261u;
    for (size_t i = 0; i < len; i++)
        a = (a ^ p[i]) * 16777619u;
    return a;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void name_of(const uint8_t *key, char name[2 * NAME_BYTES + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < NAME_BYTES; i++) {
        name[2 * i] = hex[key[i] >> 4];
        name[2 * i + 1] = hex[key[i] & 15];
    }
    name[2 * NAME_BYTES] = 0;
}

static int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* n bytes from 2n hexadecimal digits at p. */
static bool hex_bytes(const char *p, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int hi = hex_digit(p[2 * i]), lo = hex_digit(p[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static Entry *find(AuScriptCache *c, const uint8_t *key)
{
    for (size_t i = 0; i < c->n; i++)
        if (!memcmp(c->e[i].key, key, c->e[i].known ? KEY : NAME_BYTES))
            return &c->e[i];
    return NULL;
}

/* Removes the entry and its file. */
static void drop(AuScriptCache *c, Entry *e)
{
    if (e->file) {
        if (c->has_files)
            c->pending -= e->size;
        au_free(e->file);
    } else if (c->has_files) {
        char name[2 * NAME_BYTES + 1];
        name_of(e->key, name);
        c->files.remove(c->files.ctx, name);
    }
    c->bytes -= e->size;
    *e = c->e[--c->n];
}

static Entry *oldest(AuScriptCache *c)
{
    Entry *o = NULL;
    for (size_t i = 0; i < c->n; i++)
        if (!o || c->e[i].age < o->age)
            o = &c->e[i];
    return o;
}

static void trim(AuScriptCache *c, size_t room)
{
    while (c->n && (c->bytes > c->max || c->max - c->bytes < room))
        drop(c, oldest(c));
}

/* Writes a queued entry's file; the entry is dropped when it cannot be. */
static void write_entry(AuScriptCache *c, Entry *e)
{
    char name[2 * NAME_BYTES + 1];
    name_of(e->key, name);
    bool ok = c->files.write(c->files.ctx, name, e->file, e->size);
    c->pending -= e->size;
    au_free(e->file);
    e->file = NULL;
    if (!ok) {
        c->bytes -= e->size;
        *e = c->e[--c->n];
    }
}

/* ---- The keys asked for once ---- */

static size_t seen_find(const AuScriptCache *c, const uint8_t *key)
{
    for (size_t i = 0; i < c->nseen; i++)
        if (!memcmp(c->seen[i], key, SEEN_BYTES))
            return i;
    return (size_t)-1;
}

static void seen_remove(AuScriptCache *c, size_t i)
{
    memmove(c->seen[i], c->seen[i + 1], (c->nseen - i - 1) * SEEN_BYTES);
    c->nseen--;
    c->dirty = true;
}

static void seen_add(AuScriptCache *c, const uint8_t *key)
{
    if (c->nseen == AU_SCRIPT_CACHE_SEEN)
        c->nseen--;
    memmove(c->seen[1], c->seen[0], c->nseen * SEEN_BYTES);
    memcpy(c->seen[0], key, SEEN_BYTES);
    c->nseen++;
    c->dirty = true;
}

/* ---- The store ---- */

/* The bytecode in a file's bytes, checked against key; NULL when the
 * file is not this script's or is damaged. */
static const uint8_t *file_code(const uint8_t *f, size_t size, const uint8_t *key, size_t *len)
{
    if (size < FILE_HEAD || memcmp(f, MAGIC, sizeof MAGIC) || memcmp(f + sizeof MAGIC, key, KEY))
        return NULL;
    uint32_t n = get32(f + sizeof MAGIC + KEY);
    if (n != size - FILE_HEAD || get32(f + sizeof MAGIC + KEY + 4) != checksum(f + FILE_HEAD, n))
        return NULL;
    *len = n;
    return f + FILE_HEAD;
}

static uint8_t *store_get(void *ctx, const uint8_t key[KEY], size_t *len, bool *seen)
{
    AuScriptCache *c = ctx;
    *seen = false;
    Entry *e = c->max ? find(c, key) : NULL;
    if (e) {
        AuBuf b;
        au_buf_init(&b);
        const uint8_t *f = e->file;
        size_t size = e->size;
        if (!f) {
            char name[2 * NAME_BYTES + 1];
            name_of(key, name);
            if (c->files.read(c->files.ctx, name, &b)) {
                f = (const uint8_t *)b.data;
                size = b.len;
            }
        }
        size_t n = 0;
        const uint8_t *code = f ? file_code(f, size, key, &n) : NULL;
        uint8_t *out = code ? au_try_alloc(n ? n : 1) : NULL;
        if (out) {
            memcpy(out, code, n);
            *len = n;
            *seen = true;
            memcpy(e->key, key, KEY);
            e->known = true;
            c->st.hits++;
        } else if (!code) {
            /* damaged, or another script whose key starts the same: the
             * next compile stores this one in its place */
            c->st.damaged++;
            drop(c, e);
            *seen = true;
        }
        au_buf_free(&b);
        if (out || !code)
            return out;
    }
    c->st.misses++;
    if (!c->max)
        return NULL;
    size_t i = seen_find(c, key);
    if (i != (size_t)-1)
        *seen = true;
    else
        seen_add(c, key);
    return NULL;
}

static void store_put(void *ctx, const uint8_t key[KEY], const uint8_t *code, size_t len)
{
    AuScriptCache *c = ctx;
    if (len > UINT32_MAX - FILE_HEAD || len + FILE_HEAD > c->max || len + FILE_HEAD >= AU_SCRIPT_CACHE_FILE_MAX)
        return;
    size_t size = len + FILE_HEAD;
    Entry *e = find(c, key);
    if (e)
        drop(c, e);
    trim(c, size);
    uint8_t *f = au_try_alloc(size);
    if (!f)
        return;
    memcpy(f, MAGIC, sizeof MAGIC);
    memcpy(f + sizeof MAGIC, key, KEY);
    put32(f + sizeof MAGIC + KEY, (uint32_t)len);
    put32(f + sizeof MAGIC + KEY + 4, checksum(code, len));
    memcpy(f + FILE_HEAD, code, len);
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 16;
        c->e = au_realloc(c->e, c->cap * sizeof *c->e);
    }
    e = &c->e[c->n++];
    memcpy(e->key, key, KEY);
    e->size = size;
    e->age = ++c->clock;
    e->file = f;
    e->known = true;
    c->bytes += size;
    c->st.stores++;
    size_t i = seen_find(c, key);
    if (i != (size_t)-1)
        seen_remove(c, i);
    if (!c->has_files)
        return;
    c->pending += size;
    if (!c->deferred)
        write_entry(c, e);
    else if (c->pending > AU_SCRIPT_CACHE_PENDING_MAX)
        au_script_cache_flush(c);
}

/* ---- Life cycle ---- */

static void each_file(void *ctx, const char *name, size_t size, uint32_t mtime)
{
    AuScriptCache *c = ctx;
    uint8_t key[NAME_BYTES];
    if (strlen(name) != 2 * NAME_BYTES || !hex_bytes(name, key, NAME_BYTES)) {
        /* not a script's file: the Index, or something left behind */
        if (strcmp(name, AU_SCRIPT_CACHE_INDEX))
            c->files.remove(c->files.ctx, name);
        return;
    }
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 16;
        c->e = au_realloc(c->e, c->cap * sizeof *c->e);
    }
    Entry *e = &c->e[c->n++];
    memset(e, 0, sizeof *e);
    memcpy(e->key, key, NAME_BYTES);
    e->size = size;
    e->age = mtime;
    c->bytes += size;
    if (mtime > c->clock)
        c->clock = mtime;
}

AuScriptCache *au_script_cache_new(const AuDiskCacheFiles *files, size_t max_bytes)
{
    AuScriptCache *c = au_calloc(sizeof *c);
    c->max = max_bytes;
    c->store.ctx = c;
    c->store.get = store_get;
    c->store.put = store_put;
    if (files) {
        c->files = *files;
        c->has_files = true;
        c->files.list(c->files.ctx, each_file, c);
        AuBuf b;
        au_buf_init(&b);
        if (c->files.read(c->files.ctx, AU_SCRIPT_CACHE_INDEX, &b))
            au_script_cache_index_parse(c, (const char *)b.data, b.len);
        au_buf_free(&b);
        c->dirty = false;
        trim(c, 0);
    }
    return c;
}

void au_script_cache_free(AuScriptCache *c)
{
    if (!c)
        return;
    au_script_cache_flush(c);
    for (size_t i = 0; i < c->n; i++)
        au_free(c->e[i].file);
    au_free(c->e);
    au_free(c);
}

void au_script_cache_set_max(AuScriptCache *c, size_t max_bytes)
{
    c->max = max_bytes;
    trim(c, 0);
    if (!max_bytes && c->nseen) {
        c->nseen = 0;
        c->dirty = true;
    }
}

void au_script_cache_clear(AuScriptCache *c)
{
    while (c->n)
        drop(c, &c->e[c->n - 1]);
    if (c->nseen) {
        c->nseen = 0;
        c->dirty = true;
    }
}

const AuJsCodeStore *au_script_cache_store(AuScriptCache *c)
{
    return &c->store;
}

void au_script_cache_set_deferred(AuScriptCache *c, bool on)
{
    c->deferred = on;
    if (!on)
        au_script_cache_flush(c);
}

size_t au_script_cache_pending(const AuScriptCache *c)
{
    return c->pending;
}

bool au_script_cache_idle(AuScriptCache *c, int max_writes)
{
    if (!c->has_files)
        return false;
    while (c->pending && max_writes-- > 0) {
        /* the oldest queued first */
        Entry *o = NULL;
        for (size_t i = 0; i < c->n; i++)
            if (c->e[i].file && (!o || c->e[i].age < o->age))
                o = &c->e[i];
        if (!o)
            break;
        write_entry(c, o);
    }
    return c->pending > 0;
}

void au_script_cache_flush(AuScriptCache *c)
{
    while (au_script_cache_idle(c, 16)) {
    }
}

bool au_script_cache_dirty(const AuScriptCache *c)
{
    return c->has_files && c->dirty;
}

void au_script_cache_save(AuScriptCache *c)
{
    if (!c->has_files || !c->dirty)
        return;
    if (!c->nseen) {
        c->files.remove(c->files.ctx, AU_SCRIPT_CACHE_INDEX);
        c->dirty = false;
        return;
    }
    AuBuf b;
    au_buf_init(&b);
    au_script_cache_index_serialize(c, &b);
    if (c->files.write(c->files.ctx, AU_SCRIPT_CACHE_INDEX, b.data, b.len))
        c->dirty = false;
    au_buf_free(&b);
}

void au_script_cache_stats(const AuScriptCache *c, AuScriptCacheStats *out)
{
    *out = c->st;
    out->count = c->n;
    out->bytes = c->bytes;
    out->seen = c->nseen;
}

/* ---- The Index file ---- */

#define HEADER "# Aura script cache 1\n"

void au_script_cache_index_serialize(const AuScriptCache *c, AuBuf *out)
{
    static const char hex[] = "0123456789abcdef";
    au_buf_puts(out, HEADER);
    for (size_t i = 0; i < c->nseen; i++) {
        for (int k = 0; k < SEEN_BYTES; k++) {
            au_buf_putc(out, hex[c->seen[i][k] >> 4]);
            au_buf_putc(out, hex[c->seen[i][k] & 15]);
        }
        au_buf_putc(out, '\n');
    }
}

bool au_script_cache_index_parse(AuScriptCache *c, const char *data, size_t len)
{
    size_t hl = sizeof HEADER - 1;
    if (len < hl || memcmp(data, HEADER, hl))
        return false;
    const char *p = data + hl, *end = data + len;
    c->nseen = 0;
    while (p < end && c->nseen < AU_SCRIPT_CACHE_SEEN) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *e = nl ? nl : end;
        uint8_t key[SEEN_BYTES];
        if ((size_t)(e - p) == 2 * SEEN_BYTES && hex_bytes(p, key, SEEN_BYTES) && seen_find(c, key) == (size_t)-1)
            memcpy(c->seen[c->nseen++], key, SEEN_BYTES);
        p = nl ? nl + 1 : end;
    }
    c->dirty = true;
    return true;
}
