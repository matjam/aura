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
 *   about 32 KB, sized to a whole number of blocks and at least 4: 13
 *   blocks of 2.5 KB, 8 of 4 KB, 4 of 16 KB. An empty mid slab is kept only while its class
 *   has no other empty slab and fewer than AU_HEAP_MID_KEEP are kept in
 *   all; otherwise it goes back to the platform. So one long-lived block
 *   pins at most one slab of its class.
 * - au_heap_trim returns every empty slab kept; au_alloc calls it before
 *   the application's release handler when the platform has no memory.
 *
 * Larger blocks go to the platform one by one.
 *
 * Zones: every block belongs to the page zone or the long-lived zone,
 * chosen by au_heap_zone_set when it is allocated (core.h, "Zones"). The two
 * zones never share a slab, and the platform is told the zone of each
 * slab and large block it gives (get's how), so that it can keep
 * long-lived blocks apart from a page's and a closed page leaves its
 * space in one piece.
 *
 * Every block has a header of two words before it: the slab (for a large
 * block, LARGE_PAGE or LARGE_LONG) and the size asked for. Blocks are
 * aligned to two words (8 bytes on Mac OS 9, 16 on a 64-bit host).
 * Nothing here depends on byte order. */
#include "core.h"

#include <stdint.h>
#include <string.h>

#define AU_HEAP_SLAB 32768u     /* a small class's slab */
#define AU_HEAP_SMALL 2048u     /* the largest small block */
#define AU_HEAP_MID 16400u      /* the largest block from a slab */
#define AU_HEAP_MID_SLAB 32768u /* what a mid slab is sized near */
#define AU_HEAP_KEEP 8          /* empty small slabs kept, per zone */
#define AU_HEAP_MID_KEEP 4      /* empty mid slabs kept per zone, at most one per class */
#define AU_HEAP_NSMALL 32
#define AU_HEAP_CLASSES 44
#define AU_HEAP_ZONES 2

typedef struct HeapSlab HeapSlab;

/* The slab field of a large block's header: which zone it is in. */
#define LARGE_PAGE ((HeapSlab *)0)
#define LARGE_LONG ((HeapSlab *)1)
#define IS_LARGE(s) ((uintptr_t)(s) <= 1)

typedef struct HeapHdr {
    HeapSlab *slab; /* LARGE_PAGE or LARGE_LONG: a large block from the platform */
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
    uint8_t zone;
    bool listed;           /* on its class's list */
};

/* The zone of the next allocation (core.h). Code that runs before any
 * call site has set it is the application starting: long-lived. */
static unsigned char zone_cur = AU_HEAP_ZONE_LONG;

typedef struct HeapZone {
    HeapSlab *avail[AU_HEAP_CLASSES]; /* slabs with room, per class */
    HeapSlab *empty;                  /* unused small slabs */
    uint32_t nempty;
    HeapSlab *mid_empty;              /* unused mid slabs, one per class at most */
    uint32_t nmid_empty;
} HeapZone;

static struct {
    void *(*get)(size_t n, unsigned how);
    void (*release)(void *p);
    HeapZone z[AU_HEAP_ZONES];
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

void au_heap_enable(void *(*get)(size_t n, unsigned how), void (*release)(void *p))
{
    H.get = get;
    H.release = release;
}

void au_heap_stats(AuHeapStats *s)
{
    *s = H.st;
}

unsigned au_heap_zone_set(unsigned zone)
{
    unsigned prev = zone_cur;
    zone_cur = (unsigned char)(zone ? AU_HEAP_ZONE_LONG : AU_HEAP_ZONE_PAGE);
    return prev;
}

/* A held zone: -1 when none. */
static int zone_held = -1;

int au_heap_zone_hold(unsigned zone)
{
    int prev = zone_held;
    zone_held = zone ? (int)AU_HEAP_ZONE_LONG : (int)AU_HEAP_ZONE_PAGE;
    return prev;
}

void au_heap_zone_unhold(int prev)
{
    zone_held = prev;
}

/* The zone the next block goes to. */
static unsigned zone_now(void)
{
    if (zone_held >= 0)
        return (unsigned)zone_held;
    return zone_cur ? AU_HEAP_ZONE_LONG : AU_HEAP_ZONE_PAGE;
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
    HeapSlab **head = &H.z[s->zone].avail[s->cls];
    s->prev = NULL;
    s->next = *head;
    if (s->next)
        s->next->prev = s;
    *head = s;
    s->listed = true;
}

static void list_remove(HeapSlab *s)
{
    if (s->prev)
        s->prev->next = s->next;
    else
        H.z[s->zone].avail[s->cls] = s->next;
    if (s->next)
        s->next->prev = s->prev;
    s->next = s->prev = NULL;
    s->listed = false;
}

static void count_platform(size_t bytes, unsigned zone, bool add)
{
    if (add) {
        H.st.system_bytes += bytes;
        if (zone)
            H.st.long_bytes += bytes;
    } else {
        H.st.system_bytes -= bytes;
        if (zone)
            H.st.long_bytes -= bytes;
    }
}

/* Gives a slab back to the platform. */
static void slab_release(HeapSlab *s)
{
    size_t bytes = (size_t)(s->end - (char *)s);
    H.st.slabs--;
    if (s->cls >= AU_HEAP_NSMALL)
        H.st.mid_slabs--;
    count_platform(bytes, s->zone, false);
    H.release(s);
}

/* Takes the kept empty mid slab of class c off its zone's pool, or NULL. */
static HeapSlab *mid_empty_take(HeapZone *z, unsigned c)
{
    HeapSlab **pp = &z->mid_empty;
    while (*pp && (*pp)->cls != c)
        pp = &(*pp)->next;
    HeapSlab *s = *pp;
    if (s) {
        *pp = s->next;
        z->nmid_empty--;
        H.st.empty_bytes -= (size_t)(s->end - (char *)s);
    }
    return s;
}

static HeapSlab *slab_new(unsigned c, unsigned zone)
{
    HeapZone *z = &H.z[zone];
    HeapSlab *s;
    size_t bytes = slab_bytes(c);
    if (c < AU_HEAP_NSMALL) {
        s = z->empty;
        if (s) {
            z->empty = s->next;
            z->nempty--;
            H.st.empty_bytes -= bytes;
        }
    } else {
        s = mid_empty_take(z, c);
    }
    if (!s) {
        s = H.get(bytes, AU_HEAP_GET_SLAB | zone);
        if (!s)
            return NULL;
        H.st.slabs++;
        if (c >= AU_HEAP_NSMALL)
            H.st.mid_slabs++;
        count_platform(bytes, zone, true);
    }
    s->free = NULL;
    s->tail = (char *)s + slab_head();
    s->end = (char *)s + bytes;
    s->live = 0;
    s->cls = (uint16_t)c;
    s->zone = (uint8_t)zone;
    list_add(s);
    return s;
}

/* A slab whose last block was freed: kept for reuse, or returned. */
static void slab_empty(HeapSlab *s)
{
    HeapZone *z = &H.z[s->zone];
    if (s->listed)
        list_remove(s);
    size_t bytes = (size_t)(s->end - (char *)s);
    if (s->cls < AU_HEAP_NSMALL) {
        if (z->nempty < AU_HEAP_KEEP) {
            s->next = z->empty;
            z->empty = s;
            z->nempty++;
            H.st.empty_bytes += bytes;
            return;
        }
    } else if (z->nmid_empty < AU_HEAP_MID_KEEP) {
        HeapSlab *k = z->mid_empty;
        while (k && k->cls != s->cls)
            k = k->next;
        if (!k) {
            s->next = z->mid_empty;
            z->mid_empty = s;
            z->nmid_empty++;
            H.st.empty_bytes += bytes;
            return;
        }
    }
    slab_release(s);
}

size_t au_heap_trim(void)
{
    size_t freed = 0;
    for (int i = 0; i < AU_HEAP_ZONES; i++) {
        HeapZone *z = &H.z[i];
        while (z->empty) {
            HeapSlab *s = z->empty;
            z->empty = s->next;
            freed += (size_t)(s->end - (char *)s);
            slab_release(s);
        }
        while (z->mid_empty) {
            HeapSlab *s = z->mid_empty;
            z->mid_empty = s->next;
            freed += (size_t)(s->end - (char *)s);
            slab_release(s);
        }
        z->nempty = z->nmid_empty = 0;
    }
    H.st.empty_bytes = 0;
    return freed;
}

void *au_heap_alloc(size_t n)
{
    unsigned zone = zone_now();
    if (n > AU_HEAP_MID) {
        if (n > (size_t)-1 - sizeof(HeapHdr))
            return NULL;
        HeapHdr *h = H.get(n + sizeof(HeapHdr), zone);
        if (!h)
            return NULL;
        h->slab = zone ? LARGE_LONG : LARGE_PAGE;
        h->size = n;
        H.st.large++;
        count_platform(n + sizeof(HeapHdr), zone, true);
        H.st.used_bytes += n;
        H.st.blocks++;
        return h + 1;
    }
    unsigned c = class_of(n);
    HeapSlab *s = H.z[zone].avail[c];
    if (!s) {
        s = slab_new(c, zone);
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
    if (IS_LARGE(s)) {
        H.st.large--;
        count_platform(h->size + sizeof(HeapHdr), s == LARGE_LONG, false);
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

unsigned au_heap_zone_of(const void *p)
{
    const HeapHdr *h = (const HeapHdr *)p - 1;
    if (IS_LARGE(h->slab))
        return h->slab == LARGE_LONG ? AU_HEAP_ZONE_LONG : AU_HEAP_ZONE_PAGE;
    return h->slab->zone;
}

void au_heap_walk_platform_block(void *pb, unsigned how, void (*fn)(void *ctx, void *block, size_t n), void *ctx)
{
    if (!(how & AU_HEAP_GET_SLAB)) {
        HeapHdr *h = pb;
        fn(ctx, h + 1, h->size);
        return;
    }
    HeapSlab *s = pb;
    if (s->live == 0)
        return;
    size_t bs = block_size(s->cls);
    char *first = (char *)s + slab_head();
    size_t slots = (size_t)(s->tail - first) / bs;
    /* A bit per slot used; the freed ones are cleared from the free list. */
    unsigned char used[(AU_HEAP_SLAB / 16 + 7) / 8 + 1];
    if (slots > sizeof used * 8)
        slots = sizeof used * 8;
    memset(used, 0xFF, sizeof used);
    for (HeapFree *f = s->free; f; f = f->next) {
        size_t i = (size_t)((char *)f - first) / bs;
        if (i < slots)
            used[i / 8] &= (unsigned char)~(1u << (i % 8));
    }
    for (size_t i = 0; i < slots; i++)
        if (used[i / 8] & (1u << (i % 8))) {
            HeapHdr *h = (HeapHdr *)(first + i * bs);
            fn(ctx, h + 1, h->size);
        }
}

void *au_heap_realloc(void *p, size_t n)
{
    if (!p)
        return au_heap_alloc(n);
    HeapHdr *h = (HeapHdr *)p - 1;
    size_t old = h->size;
    if (!IS_LARGE(h->slab) && n <= AU_HEAP_MID && class_of(n) == h->slab->cls) {
        H.st.used_bytes += n;
        H.st.used_bytes -= old;
        h->size = n;
        return p;
    }
    /* A large block that shrinks by less than half stays where it is
     * (the statistics then count it at its new size). */
    if (IS_LARGE(h->slab) && n <= old && n > AU_HEAP_MID && n >= old / 2) {
        H.st.used_bytes -= old - n;
        count_platform(old - n, h->slab == LARGE_LONG, false);
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
