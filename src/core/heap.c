/* The pooled allocator behind au_alloc when a platform turns it on
 * (au_heap_enable, core.h).
 *
 * Why: on Mac OS 9, Retro68's malloc is one NewPtr per block, and the
 * Memory Manager searches its block list for each one. A large page holds
 * about 90,000 blocks and a relayout makes about 30,000 allocations, which
 * took tens of seconds there. The script engine adds thousands of 4 KB
 * arenas, which it frees as soon as they are empty. This allocator asks
 * the platform only for slabs and for blocks over AU_HEAP_MID bytes.
 *
 * How: blocks up to AU_HEAP_MID bytes come from slabs. A slab serves one
 * size class; it hands out blocks from its free list, else from its
 * unused tail. Each class keeps a list of its slabs that have room.
 *
 * - Small classes (up to AU_HEAP_SMALL bytes) use 32 KB slabs. A small
 *   slab whose last block is freed goes to a pool of empty slabs that any
 *   small class takes from; the pool keeps at most AU_HEAP_KEEP slabs and
 *   returns the rest to the platform.
 * - Mid classes (over AU_HEAP_SMALL, up to AU_HEAP_MID) use slabs of
 *   about 64 KB, sized to a whole number of blocks: 26 blocks of 2.5 KB,
 *   16 of 4 KB, 4 of 16 KB. An empty mid slab is kept only while its class
 *   has no other empty slab and fewer than AU_HEAP_MID_KEEP are kept in
 *   all; otherwise it goes back to the platform. So one long-lived block
 *   pins at most one slab of its class.
 * - au_heap_trim returns every empty slab kept; au_alloc calls it before
 *   the application's release handler when the platform has no memory.
 *
 * Larger blocks go to the platform one by one.
 *
 * Every block has a header of two words before it: the slab (NULL for a
 * large block) and the size asked for. Blocks are aligned to two words
 * (8 bytes on Mac OS 9, 16 on a 64-bit host). Nothing here depends on byte
 * order. */
#include "core.h"

#include <string.h>

#define AU_HEAP_SLAB 32768u     /* a small class's slab */
#define AU_HEAP_SMALL 2048u     /* the largest small block */
#define AU_HEAP_MID 16400u      /* the largest block from a slab */
#define AU_HEAP_MID_SLAB 65536u /* what a mid slab is sized near */
#define AU_HEAP_KEEP 8          /* empty small slabs kept */
#define AU_HEAP_MID_KEEP 4      /* empty mid slabs kept, at most one per class */
#define AU_HEAP_NSMALL 32
#define AU_HEAP_CLASSES 44

typedef struct HeapSlab HeapSlab;

typedef struct HeapHdr {
    HeapSlab *slab; /* NULL: a large block from the platform */
    size_t size;    /* bytes asked for */
} HeapHdr;

typedef struct HeapFree {
    struct HeapFree *next;
} HeapFree;

struct HeapSlab {
    HeapSlab *next, *prev; /* in its class's list of slabs with room, or an empty pool (next only) */
    HeapFree *free;        /* freed blocks (each a whole block, header included) */
    char *tail, *end;      /* never used yet; the slab's end */
    uint32_t live;         /* blocks in use */
    uint16_t cls;
    bool listed;           /* on its class's list */
};

static struct {
    void *(*get)(size_t n);
    void (*release)(void *p);
    HeapSlab *avail[AU_HEAP_CLASSES]; /* slabs with room, per class */
    HeapSlab *empty;                  /* unused small slabs */
    uint32_t nempty;
    HeapSlab *mid_empty;              /* unused mid slabs, one per class at most */
    uint32_t nmid_empty;
    AuHeapStats st;
} H;

/* Payload sizes: every 8 bytes to 128, then four steps per doubling to
 * 2 KB (the small classes), then four steps per doubling to 16 KB. The
 * mid classes at a power of two are 16 bytes over it, so that a 4 KB
 * request with a 16-byte header of its caller's (the script engine's
 * arenas, src/js/runtime.c) still fits. */
static const uint16_t class_size[AU_HEAP_CLASSES] = {
    8,    16,   24,   32,   40,   48,    56,    64,    72,    80,    88,    96,    104,  112,  120,
    128,  160,  192,  224,  256,  320,   384,   448,   512,   640,   768,   896,   1024, 1280, 1536,
    1792, 2048, 2560, 3072, 3584, 4112,  5120,  6144,  7168,  8208,  10240, 12288, 14336, 16400};

static unsigned class_of(size_t n)
{
    if (n <= 128)
        return n ? (unsigned)((n - 1) >> 3) : 0;
    unsigned c = n > AU_HEAP_SMALL ? AU_HEAP_NSMALL : 16;
    while (class_size[c] < n)
        c++;
    return c;
}

/* The size of a block of class c with its header, rounded to the
 * alignment. */
static size_t block_size(unsigned c)
{
    size_t a = sizeof(HeapHdr);
    return (sizeof(HeapHdr) + class_size[c] + a - 1) & ~(a - 1);
}

static size_t slab_head(void)
{
    size_t a = sizeof(HeapHdr);
    return (sizeof(HeapSlab) + a - 1) & ~(a - 1);
}

/* The bytes of a slab of class c: 32 KB for a small class; for a mid
 * class, the whole number of blocks nearest AU_HEAP_MID_SLAB bytes, and at
 * least 4. */
static size_t slab_bytes(unsigned c)
{
    if (c < AU_HEAP_NSMALL)
        return AU_HEAP_SLAB;
    size_t bs = block_size(c);
    size_t count = (AU_HEAP_MID_SLAB + bs / 2) / bs;
    if (count < 4)
        count = 4;
    return slab_head() + count * bs;
}

bool au_heap_enabled(void)
{
    return H.get != NULL;
}

void au_heap_enable(void *(*get)(size_t n), void (*release)(void *p))
{
    H.get = get;
    H.release = release;
}

void au_heap_stats(AuHeapStats *s)
{
    *s = H.st;
}

/* Copies n bytes between two word-aligned blocks a word at a time (the
 * newlib memcpy of the Mac OS 9 build copies bytes). */
static void copy_words(void *dst, const void *src, size_t n)
{
    size_t *d = dst;
    const size_t *s = src;
    size_t w = n / sizeof(size_t);
    for (size_t i = 0; i < w; i++)
        d[i] = s[i];
    unsigned char *db = (unsigned char *)(d + w);
    const unsigned char *sb = (const unsigned char *)(s + w);
    for (size_t i = 0; i < n % sizeof(size_t); i++)
        db[i] = sb[i];
}

static void list_add(HeapSlab *s)
{
    s->prev = NULL;
    s->next = H.avail[s->cls];
    if (s->next)
        s->next->prev = s;
    H.avail[s->cls] = s;
    s->listed = true;
}

static void list_remove(HeapSlab *s)
{
    if (s->prev)
        s->prev->next = s->next;
    else
        H.avail[s->cls] = s->next;
    if (s->next)
        s->next->prev = s->prev;
    s->next = s->prev = NULL;
    s->listed = false;
}

/* Gives a slab back to the platform. */
static void slab_release(HeapSlab *s)
{
    size_t bytes = (size_t)(s->end - (char *)s);
    H.st.slabs--;
    if (s->cls >= AU_HEAP_NSMALL)
        H.st.mid_slabs--;
    H.st.system_bytes -= bytes;
    H.release(s);
}

/* Takes the kept empty mid slab of class c off its pool, or NULL. */
static HeapSlab *mid_empty_take(unsigned c)
{
    HeapSlab **pp = &H.mid_empty;
    while (*pp && (*pp)->cls != c)
        pp = &(*pp)->next;
    HeapSlab *s = *pp;
    if (s) {
        *pp = s->next;
        H.nmid_empty--;
        H.st.empty_bytes -= (size_t)(s->end - (char *)s);
    }
    return s;
}

static HeapSlab *slab_new(unsigned c)
{
    HeapSlab *s;
    size_t bytes = slab_bytes(c);
    if (c < AU_HEAP_NSMALL) {
        s = H.empty;
        if (s) {
            H.empty = s->next;
            H.nempty--;
            H.st.empty_bytes -= bytes;
        }
    } else {
        s = mid_empty_take(c);
    }
    if (!s) {
        s = H.get(bytes);
        if (!s)
            return NULL;
        H.st.slabs++;
        if (c >= AU_HEAP_NSMALL)
            H.st.mid_slabs++;
        H.st.system_bytes += bytes;
    }
    s->free = NULL;
    s->tail = (char *)s + slab_head();
    s->end = (char *)s + bytes;
    s->live = 0;
    s->cls = (uint16_t)c;
    list_add(s);
    return s;
}

/* A slab whose last block was freed: kept for reuse, or returned. */
static void slab_empty(HeapSlab *s)
{
    if (s->listed)
        list_remove(s);
    size_t bytes = (size_t)(s->end - (char *)s);
    if (s->cls < AU_HEAP_NSMALL) {
        if (H.nempty < AU_HEAP_KEEP) {
            s->next = H.empty;
            H.empty = s;
            H.nempty++;
            H.st.empty_bytes += bytes;
            return;
        }
    } else if (H.nmid_empty < AU_HEAP_MID_KEEP) {
        HeapSlab *k = H.mid_empty;
        while (k && k->cls != s->cls)
            k = k->next;
        if (!k) {
            s->next = H.mid_empty;
            H.mid_empty = s;
            H.nmid_empty++;
            H.st.empty_bytes += bytes;
            return;
        }
    }
    slab_release(s);
}

size_t au_heap_trim(void)
{
    size_t freed = 0;
    while (H.empty) {
        HeapSlab *s = H.empty;
        H.empty = s->next;
        freed += (size_t)(s->end - (char *)s);
        slab_release(s);
    }
    while (H.mid_empty) {
        HeapSlab *s = H.mid_empty;
        H.mid_empty = s->next;
        freed += (size_t)(s->end - (char *)s);
        slab_release(s);
    }
    H.nempty = H.nmid_empty = 0;
    H.st.empty_bytes = 0;
    return freed;
}

void *au_heap_alloc(size_t n)
{
    if (n > AU_HEAP_MID) {
        if (n > (size_t)-1 - sizeof(HeapHdr))
            return NULL;
        HeapHdr *h = H.get(n + sizeof(HeapHdr));
        if (!h)
            return NULL;
        h->slab = NULL;
        h->size = n;
        H.st.large++;
        H.st.system_bytes += n + sizeof(HeapHdr);
        H.st.used_bytes += n;
        H.st.blocks++;
        return h + 1;
    }
    unsigned c = class_of(n);
    HeapSlab *s = H.avail[c];
    if (!s) {
        s = slab_new(c);
        if (!s)
            return NULL;
    }
    HeapHdr *h;
    size_t bs = block_size(c);
    if (s->free) {
        h = (HeapHdr *)s->free;
        s->free = s->free->next;
    } else {
        h = (HeapHdr *)s->tail;
        s->tail += bs;
    }
    s->live++;
    if (!s->free && (size_t)(s->end - s->tail) < bs)
        list_remove(s); /* full */
    h->slab = s;
    h->size = n;
    H.st.used_bytes += n;
    H.st.blocks++;
    if (c >= AU_HEAP_NSMALL)
        H.st.mid_blocks++;
    return h + 1;
}

void au_heap_free(void *p)
{
    if (!p)
        return;
    HeapHdr *h = (HeapHdr *)p - 1;
    HeapSlab *s = h->slab;
    H.st.used_bytes -= h->size;
    H.st.blocks--;
    if (!s) {
        H.st.large--;
        H.st.system_bytes -= h->size + sizeof(HeapHdr);
        H.release(h);
        return;
    }
    if (s->cls >= AU_HEAP_NSMALL)
        H.st.mid_blocks--;
    HeapFree *f = (HeapFree *)h;
    f->next = s->free;
    s->free = f;
    s->live--;
    if (s->live == 0)
        slab_empty(s);
    else if (!s->listed)
        list_add(s);
}

void *au_heap_realloc(void *p, size_t n)
{
    if (!p)
        return au_heap_alloc(n);
    HeapHdr *h = (HeapHdr *)p - 1;
    size_t old = h->size;
    if (h->slab && n <= AU_HEAP_MID && class_of(n) == h->slab->cls) {
        H.st.used_bytes += n;
        H.st.used_bytes -= old;
        h->size = n;
        return p;
    }
    /* A large block that shrinks by less than half stays where it is
     * (the statistics then count it at its new size). */
    if (!h->slab && n <= old && n > AU_HEAP_MID && n >= old / 2) {
        H.st.used_bytes -= old - n;
        H.st.system_bytes -= old - n;
        h->size = n;
        return p;
    }
    void *q = au_heap_alloc(n);
    if (!q)
        return NULL;
    copy_words(q, p, old < n ? old : n);
    au_heap_free(p);
    return q;
}
