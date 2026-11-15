/* The disk cache. See disk_cache.h. */
#include "disk_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC "AURD1\n"
#define MAGIC_LEN 6

typedef struct DcFile {
    char name[19];
    size_t size;
    uint32_t mtime;
    uint32_t use; /* larger: used more recently */
} DcFile;

struct AuDiskCache {
    AuDiskCacheFiles files;
    size_t max, total;
    DcFile *f;
    size_t n, cap;
    uint32_t clock;
    bool listed;
};

void au_disk_cache_name(const char *key, char out[19])
{
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    static const char hex[] = "0123456789abcdef";
    out[0] = 'c';
    for (int i = 0; i < 16; i++)
        out[1 + i] = hex[(h >> (60 - 4 * i)) & 15];
    out[17] = 0;
    out[18] = 0;
}

static bool valid_name(const char *name)
{
    if (name[0] != 'c' || strlen(name) != 17)
        return false;
    for (int i = 1; i < 17; i++)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f')))
            return false;
    return true;
}

static DcFile *find(AuDiskCache *d, const char *name)
{
    for (size_t i = 0; i < d->n; i++)
        if (!strcmp(d->f[i].name, name))
            return &d->f[i];
    return NULL;
}

static void drop_index(AuDiskCache *d, size_t i)
{
    d->total -= d->f[i].size;
    memmove(d->f + i, d->f + i + 1, (d->n - i - 1) * sizeof *d->f);
    d->n--;
}

static DcFile *add_index(AuDiskCache *d, const char *name, size_t size, uint32_t mtime)
{
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 64;
        d->f = au_realloc(d->f, d->cap * sizeof *d->f);
    }
    DcFile *f = &d->f[d->n++];
    memset(f, 0, sizeof *f);
    memcpy(f->name, name, 18);
    f->size = size;
    f->mtime = mtime;
    d->total += size;
    return f;
}

static void each_file(void *ctx, const char *name, size_t size, uint32_t mtime)
{
    AuDiskCache *d = ctx;
    if (!valid_name(name))
        return; /* not ours (the platform removes its own temporary files) */
    if (!find(d, name))
        add_index(d, name, size, mtime);
}

static int by_mtime(const void *a, const void *b)
{
    const DcFile *x = a, *y = b;
    return x->mtime < y->mtime ? -1 : x->mtime > y->mtime ? 1 : 0;
}

static void evict(AuDiskCache *d, size_t max);

static void ensure_listed(AuDiskCache *d)
{
    if (d->listed)
        return;
    d->listed = true;
    if (d->files.list)
        d->files.list(d->files.ctx, each_file, d);
    if (d->n > 1)
        qsort(d->f, d->n, sizeof *d->f, by_mtime);
    for (size_t i = 0; i < d->n; i++)
        d->f[i].use = ++d->clock;
    evict(d, d->max);
}

static void evict(AuDiskCache *d, size_t max)
{
    while (d->n && d->total > max) {
        size_t victim = 0;
        for (size_t i = 1; i < d->n; i++)
            if (d->f[i].use < d->f[victim].use)
                victim = i;
        if (d->files.remove)
            d->files.remove(d->files.ctx, d->f[victim].name);
        drop_index(d, victim);
    }
}

AuDiskCache *au_disk_cache_new(const AuDiskCacheFiles *files, size_t max_bytes)
{
    AuDiskCache *d = au_calloc(sizeof *d);
    d->files = *files;
    d->max = max_bytes;
    return d;
}

void au_disk_cache_free(AuDiskCache *d)
{
    if (!d)
        return;
    au_free(d->f);
    au_free(d);
}

static void dc_store(void *ctx, const char *key, const void *data, size_t len)
{
    AuDiskCache *d = ctx;
    ensure_listed(d);
    char name[19];
    au_disk_cache_name(key, name);
    size_t klen = strlen(key);
    size_t total = MAGIC_LEN + klen + 1 + len;
    DcFile *old = find(d, name);
    if (old)
        drop_index(d, (size_t)(old - d->f));
    if (total > d->max / 4 || !d->files.write) {
        /* Too large for the budget (or nowhere to write): no stale copy
         * may remain. */
        if (old && d->files.remove)
            d->files.remove(d->files.ctx, name);
        return;
    }
    evict(d, d->max - total);
    AuBuf b;
    au_buf_init(&b);
    au_buf_append(&b, MAGIC, MAGIC_LEN);
    au_buf_append(&b, key, klen);
    au_buf_putc(&b, '\n');
    au_buf_append(&b, data, len);
    if (d->files.write(d->files.ctx, name, b.data, b.len)) {
        DcFile *f = add_index(d, name, total, 0);
        f->use = ++d->clock;
    } else if (old && d->files.remove) {
        d->files.remove(d->files.ctx, name);
    }
    au_buf_free(&b);
}

static bool dc_load(void *ctx, const char *key, AuBuf *out)
{
    AuDiskCache *d = ctx;
    ensure_listed(d);
    char name[19];
    au_disk_cache_name(key, name);
    DcFile *f = find(d, name);
    if (!f || !d->files.read)
        return false;
    AuBuf b;
    au_buf_init(&b);
    size_t klen = strlen(key);
    bool ok = d->files.read(d->files.ctx, name, &b) && b.len >= MAGIC_LEN + klen + 1 &&
              !memcmp(b.data, MAGIC, MAGIC_LEN) && !memcmp(b.data + MAGIC_LEN, key, klen) &&
              b.data[MAGIC_LEN + klen] == '\n';
    if (ok) {
        au_buf_clear(out);
        au_buf_append(out, b.data + MAGIC_LEN + klen + 1, b.len - MAGIC_LEN - klen - 1);
        f->use = ++d->clock;
    } else {
        /* Damaged, or another key with the same hash: a miss. A damaged
         * file is removed; a collision's file is kept for its own key. */
        bool collision = b.len >= MAGIC_LEN && !memcmp(b.data, MAGIC, MAGIC_LEN) &&
                         memchr(b.data + MAGIC_LEN, '\n', b.len - MAGIC_LEN);
        if (!collision) {
            if (d->files.remove)
                d->files.remove(d->files.ctx, name);
            drop_index(d, (size_t)(f - d->f));
        }
    }
    au_buf_free(&b);
    return ok;
}

static void dc_remove(void *ctx, const char *key)
{
    AuDiskCache *d = ctx;
    ensure_listed(d);
    char name[19];
    au_disk_cache_name(key, name);
    DcFile *f = find(d, name);
    if (!f)
        return;
    if (d->files.remove)
        d->files.remove(d->files.ctx, name);
    drop_index(d, (size_t)(f - d->f));
}

void au_disk_cache_storage(AuDiskCache *d, AuCacheStorage *out)
{
    out->ctx = d;
    out->store = dc_store;
    out->load = dc_load;
    out->remove = dc_remove;
}

void au_disk_cache_set_max(AuDiskCache *d, size_t max_bytes)
{
    d->max = max_bytes;
    ensure_listed(d);
    evict(d, max_bytes);
}

void au_disk_cache_clear(AuDiskCache *d)
{
    ensure_listed(d);
    evict(d, 0);
}

size_t au_disk_cache_bytes(AuDiskCache *d)
{
    ensure_listed(d);
    return d->total;
}

size_t au_disk_cache_count(AuDiskCache *d)
{
    ensure_listed(d);
    return d->n;
}
