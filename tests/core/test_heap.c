/* The pooled allocator (src/core/heap.c) over malloc: random allocations,
 * reallocations and frees with every block's contents checked, alignment,
 * the statistics, the slabs of blocks over 2 KB (reuse, one empty slab
 * kept per class, a live block pinning one slab), au_heap_trim and the
 * release path, and that everything goes back to the platform.
 *
 * Nothing in the allocator depends on byte order; reallocations of odd
 * lengths check that the word-at-a-time copy also moves the last bytes. */
#include "core/core.h"
#include "core/region.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The platform: malloc, with a count of blocks and bytes, a failure after
 * N more calls, and a budget of bytes it may hand out. */
static size_t sys_blocks, sys_bytes, sys_fail_after = (size_t)-1, sys_budget = (size_t)-1;

typedef struct SysHdr {
    size_t n, how; /* two words: the block after it keeps malloc's alignment for the pool */
} SysHdr;

static size_t sys_long_blocks, sys_long_bytes; /* of them, the long-lived zone's */

static void *get(size_t n, unsigned how)
{
    if (sys_fail_after == 0)
        return NULL;
    if (sys_budget != (size_t)-1 && (n > sys_budget || sys_bytes > sys_budget - n))
        return NULL;
    if (sys_fail_after != (size_t)-1)
        sys_fail_after--;
    SysHdr *h = malloc(sizeof(SysHdr) + n);
    if (!h)
        return NULL;
    h->n = n;
    h->how = how;
    sys_blocks++;
    sys_bytes += n;
    if ((how & AU_HEAP_ZONE_MASK) == AU_HEAP_ZONE_LONG) {
        sys_long_blocks++;
        sys_long_bytes += n;
    }
    return h + 1;
}

static void release(void *p)
{
    SysHdr *h = (SysHdr *)p - 1;
    sys_blocks--;
    sys_bytes -= h->n;
    if ((h->how & AU_HEAP_ZONE_MASK) == AU_HEAP_ZONE_LONG) {
        sys_long_blocks--;
        sys_long_bytes -= h->n;
    }
    free(h);
}

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

typedef struct Blk {
    unsigned char *p;
    size_t n;
    unsigned char seed;
} Blk;

static void fill(Blk *b, size_t from)
{
    for (size_t i = from; i < b->n; i++)
        b->p[i] = (unsigned char)(b->seed + i * 7);
}

static bool intact(const Blk *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (b->p[i] != (unsigned char)(b->seed + i * 7))
            return false;
    return true;
}

static bool aligned(const void *p)
{
    return ((uintptr_t)p & (2 * sizeof(void *) - 1)) == 0;
}

static size_t pick_size(void)
{
    uint32_t r = rnd() % 100;
    if (r < 55)
        return 1 + rnd() % 130; /* au_alloc(0) allocates one byte */
    if (r < 80)
        return 1 + rnd() % 2100;
    if (r < 90)
        return 2000 + rnd() % 6000;
    if (r < 98)
        return 4000 + rnd() % 13000; /* around the largest class, 16400 */
    return 30000 + rnd() % 200000;
}

static AuHeapStats stats(void)
{
    AuHeapStats st;
    au_heap_stats(&st);
    return st;
}

static void random_test(void)
{
    enum { N = 4000 };
    static Blk b[N];
    for (int round = 0; round < 200000; round++) {
        Blk *k = &b[rnd() % N];
        uint32_t op = rnd() % 10;
        if (!k->p) {
            k->n = pick_size();
            k->seed = (unsigned char)rnd();
            k->p = au_alloc(k->n);
            CHECK(aligned(k->p));
            fill(k, 0);
        } else if (op < 4) {
            CHECK(intact(k, k->n));
            au_free(k->p);
            k->p = NULL;
        } else if (op < 7) {
            size_t n = pick_size(), keep = n < k->n ? n : k->n;
            k->p = au_realloc(k->p, n);
            CHECK(aligned(k->p));
            CHECK(intact(k, keep));
            k->n = n;
            fill(k, keep);
        } else {
            CHECK(intact(k, k->n));
        }
        if (fails > 10)
            return;
    }
    AuHeapStats st = stats();
    size_t live = 0, bytes = 0;
    for (int i = 0; i < N; i++)
        if (b[i].p) {
            live++;
            bytes += b[i].n;
        }
    CHECK(st.blocks == live);
    CHECK(st.used_bytes == bytes);
    CHECK(st.slabs + st.large == sys_blocks);
    CHECK(st.system_bytes == sys_bytes);
    for (int i = 0; i < N; i++)
        if (b[i].p) {
            CHECK(intact(&b[i], b[i].n));
            au_free(b[i].p);
            b[i].p = NULL;
        }
    st = stats();
    CHECK(st.blocks == 0 && st.used_bytes == 0 && st.large == 0 && st.mid_blocks == 0);
    /* Only the empty slabs kept are still held: 8 small, 4 mid. */
    CHECK(st.slabs <= 8 + 4 && sys_blocks == st.slabs);
    CHECK(st.empty_bytes == st.system_bytes);
}

/* Every mid class: alignment, the whole block writable, and the class
 * boundaries (16400 from a slab, 16401 from the platform). */
static void mid_classes(void)
{
    static const size_t sizes[] = {2049, 2560, 2561, 3000, 3584, 4096, 4112, 4113, 5120, 6000,
                                   7168, 8192, 8208, 9000, 12288, 14000, 16384, 16400};
    enum { NS = sizeof sizes / sizeof sizes[0] };
    Blk b[NS];
    AuHeapStats before = stats();
    for (int i = 0; i < NS; i++) {
        b[i].n = sizes[i];
        b[i].seed = (unsigned char)(i * 31);
        b[i].p = au_alloc(b[i].n);
        CHECK(aligned(b[i].p));
        fill(&b[i], 0);
    }
    AuHeapStats st = stats();
    CHECK(st.mid_blocks == before.mid_blocks + NS);
    CHECK(st.large == before.large);
    void *big = au_alloc(16401);
    CHECK(stats().large == before.large + 1);
    au_free(big);
    for (int i = 0; i < NS; i++) {
        CHECK(intact(&b[i], b[i].n));
        au_free(b[i].p);
    }
    CHECK(stats().mid_blocks == before.mid_blocks);
}

/* The script engine's 4 KB arenas (4096 bytes and its 16-byte header):
 * 8 to a slab, a freed block reused, one empty slab of the class kept. */
static void arenas(void)
{
    au_heap_trim();
    size_t blocks0 = sys_blocks;
    enum { NA = 150, PER = 8 };
    void *a[NA];
    for (int i = 0; i < NA; i++) {
        a[i] = au_alloc(4112);
        CHECK(aligned(a[i]));
        memset(a[i], 0xA5, 4112);
    }
    CHECK(sys_blocks - blocks0 == (NA + PER - 1) / PER);
    CHECK(stats().mid_slabs == (NA + PER - 1) / PER);

    /* A freed block is the next one handed out. */
    void *k = a[37];
    au_free(k);
    a[37] = au_alloc(4100);
    CHECK(a[37] == k);

    /* Growing within the class stays; past it the block moves, with its
     * bytes, odd lengths included. */
    unsigned char *r = au_realloc(a[37], 4112);
    CHECK(r == k);
    for (int i = 0; i < 4111; i++)
        r[i] = (unsigned char)(i * 13 + 1);
    r = au_realloc(r, 6001);
    CHECK(r != k);
    bool same = true;
    for (int i = 0; i < 4111; i++)
        same = same && r[i] == (unsigned char)(i * 13 + 1);
    CHECK(same);
    r = au_realloc(r, 99); /* to a small class */
    same = true;
    for (int i = 0; i < 99; i++)
        same = same && r[i] == (unsigned char)(i * 13 + 1);
    CHECK(same);
    au_free(r);
    a[37] = au_alloc(4112);
    au_heap_trim(); /* the empty slabs of the 6144 and small classes */

    /* All but one freed: that block pins its slab, and one empty slab of
     * the class is kept; the rest go back to the platform. */
    for (int i = 1; i < NA; i++)
        au_free(a[i]);
    AuHeapStats st = stats();
    CHECK(st.mid_blocks == 1);
    CHECK(st.mid_slabs == 2);
    CHECK(sys_blocks - blocks0 == 2);
    au_free(a[0]);
    st = stats();
    CHECK(st.mid_slabs == 1 && st.mid_blocks == 0);
    CHECK(sys_blocks - blocks0 == 1);
}

/* At most AU_HEAP_MID_KEEP (4) empty mid slabs are kept, one per class. */
static void keep_limit(void)
{
    au_heap_trim();
    static const size_t sizes[] = {2500, 3000, 3500, 4100, 5000, 6000, 7000, 8000};
    enum { NS = sizeof sizes / sizeof sizes[0] };
    void *p[NS];
    for (int i = 0; i < NS; i++)
        p[i] = au_alloc(sizes[i]);
    CHECK(stats().mid_slabs == NS);
    for (int i = 0; i < NS; i++)
        au_free(p[i]);
    AuHeapStats st = stats();
    CHECK(st.mid_slabs == 4);
    CHECK(st.empty_bytes == st.system_bytes);
    size_t freed = au_heap_trim();
    CHECK(freed == st.empty_bytes);
    st = stats();
    CHECK(st.slabs == 0 && st.empty_bytes == 0 && st.system_bytes == 0 && sys_blocks == 0);
}

/* Zones (core.h): long-lived blocks never share a slab with page blocks,
 * the platform is told each slab's and large block's zone, a held zone
 * wins over the files' zones, and the statistics count the long-lived
 * zone's platform bytes. */
static void zones(void)
{
    au_heap_trim();
    size_t blocks0 = sys_blocks, long0 = sys_long_blocks;
    CHECK(stats().long_bytes == sys_long_bytes);

    /* One page block and one long-lived block of the same class: two
     * slabs, one of each zone. */
    void *pg = au_alloc(100);
    void *lg = au_alloc_long(100);
    CHECK(au_heap_zone_of(pg) == AU_HEAP_ZONE_PAGE);
    CHECK(au_heap_zone_of(lg) == AU_HEAP_ZONE_LONG);
    CHECK(sys_blocks - blocks0 == 2 && sys_long_blocks - long0 == 1);
    CHECK(stats().long_bytes == sys_long_bytes);

    /* More of each fill their own slabs. */
    void *pg2 = au_alloc(100), *lg2 = au_calloc_long(100);
    CHECK(sys_blocks - blocks0 == 2);
    CHECK(au_heap_zone_of(pg2) == AU_HEAP_ZONE_PAGE && au_heap_zone_of(lg2) == AU_HEAP_ZONE_LONG);
    CHECK(((unsigned char *)lg2)[99] == 0);

    /* A large block and a mid one: the platform hears the zone. */
    void *big = au_alloc_long(50000);
    CHECK(au_heap_zone_of(big) == AU_HEAP_ZONE_LONG && sys_long_blocks - long0 == 2);
    void *mid = au_alloc_long(4112);
    CHECK(au_heap_zone_of(mid) == AU_HEAP_ZONE_LONG && sys_long_blocks - long0 == 3);
    CHECK(stats().long_bytes == sys_long_bytes);

    /* Growing a long-lived block past its class keeps it long-lived. */
    lg2 = au_realloc_long(lg2, 3000);
    CHECK(au_heap_zone_of(lg2) == AU_HEAP_ZONE_LONG);

    /* A held zone wins over the zone each call site sets. */
    int h = au_heap_zone_hold(AU_HEAP_ZONE_LONG);
    void *held = au_alloc(200);
    char *held_s = au_strndup("long", 4);
    au_heap_zone_unhold(h);
    void *after = au_alloc(200);
    CHECK(au_heap_zone_of(held) == AU_HEAP_ZONE_LONG && au_heap_zone_of(held_s) == AU_HEAP_ZONE_LONG);
    CHECK(au_heap_zone_of(after) == AU_HEAP_ZONE_PAGE);

    /* au_heap_zone_set returns the zone before it. */
    unsigned prev = au_heap_zone_set(AU_HEAP_ZONE_LONG);
    CHECK(prev == AU_HEAP_ZONE_PAGE);
    CHECK(au_heap_zone_set(prev) == AU_HEAP_ZONE_LONG);

    void *all[] = {pg, lg, pg2, lg2, big, mid, held, held_s, after};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
        au_free(all[i]);
    au_heap_trim();
    CHECK(sys_blocks == blocks0 && sys_long_blocks == long0);
    CHECK(stats().long_bytes == sys_long_bytes);
}

static void region_count(void *ctx, void *p, size_t n, bool is_free)
{
    int *w = ctx; /* live, free */
    (void)p;
    (void)n;
    w[is_free ? 1 : 0]++;
}

/* The region allocator (core/region.h): low and high placement, joining
 * freed neighbours, chunks, the walk, and taking out an empty chunk. */
static void region(void)
{
    enum { CH = 64 * 1024 };
    static size_t mem[3][CH / sizeof(size_t)];
    AuRegion r;
    au_region_init(&r);
    CHECK(au_region_alloc(&r, 16, false) == NULL);
    CHECK(au_region_add(&r, mem[0], CH));
    size_t free0, big0;
    au_region_free_space(&r, &free0, &big0);
    CHECK(free0 == big0 && free0 > CH - 64);

    /* Low blocks go up from the bottom, high ones down from the top. */
    char *a = au_region_alloc(&r, 1000, false);
    char *b = au_region_alloc(&r, 1000, false);
    char *c = au_region_alloc(&r, 1000, true);
    char *d = au_region_alloc(&r, 1000, true);
    CHECK(a && b && c && d);
    CHECK(a < b && b < d && d < c);
    CHECK(aligned(a) && aligned(b) && aligned(c) && aligned(d));
    CHECK(c + au_region_size(c) + 64 > (char *)mem[0] + CH);
    CHECK(au_region_owns(&r, a) && !au_region_owns(&r, mem[1]));
    CHECK(au_region_size(a) >= 1000 && *au_region_tag(a) == 0);
    *au_region_tag(a) = 77;
    memset(a, 1, 1000);
    memset(b, 2, 1000);
    memset(c, 3, 1000);
    memset(d, 4, 1000);
    CHECK(r.blocks == 4);

    /* A freed block is used again by the next low block that fits. */
    au_region_free(&r, a);
    char *a2 = au_region_alloc(&r, 900, false);
    CHECK(a2 == a && *au_region_tag(a2) == 0);

    /* Freeing joins neighbours: all freed, one extent again. */
    au_region_free(&r, b);
    au_region_free(&r, a2);
    au_region_free(&r, d);
    au_region_free(&r, c);
    size_t f, big;
    au_region_free_space(&r, &f, &big);
    CHECK(f == free0 && big == big0 && r.blocks == 0 && r.used == 0);

    /* Too large for any extent. */
    CHECK(au_region_alloc(&r, CH, false) == NULL);

    /* A second chunk, right after the first in memory: free space at the
     * end of one is not joined with the start of the other. The walk
     * visits both in order, free blocks too. */
    CHECK(au_region_add(&r, mem[1], CH));
    char *x = au_region_alloc(&r, CH - 4096, false); /* fills most of the first */
    char *y = au_region_alloc(&r, 8000, false);      /* the second */
    CHECK(x && y && au_region_owns(&r, y) && (size_t)(y - (char *)mem[1]) < 64);
    int w[2] = {0, 0};
    au_region_walk(&r, region_count, w);
    CHECK(w[0] == 2 && w[1] == 2);

    /* An empty chunk can be taken out; one with a block cannot. */
    CHECK(au_region_drop_free_chunk(&r) == -1);
    au_region_free(&r, y);
    CHECK(au_region_drop_free_chunk(&r) == 1 && r.nchunks == 1);
    CHECK(au_region_drop_free_chunk(&r) == -1);
    au_region_free(&r, x);
    CHECK(au_region_drop_free_chunk(&r) == 0 && r.nchunks == 0 && r.free == NULL && r.total == 0);
}

/* au_region_resize: a shrink frees the tail (joined with free space after
 * it), a growth takes the free block after it and only that, and the
 * bytes kept are untouched. */
static void region_resize(void)
{
    enum { CH = 64 * 1024 };
    static size_t mem[CH / sizeof(size_t)];
    AuRegion r;
    au_region_init(&r);
    CHECK(au_region_add(&r, mem, CH));
    size_t free0;
    au_region_free_space(&r, &free0, NULL);
    char *a = au_region_alloc(&r, 4000, false);
    char *b = au_region_alloc(&r, 4000, false);
    CHECK(a && b && a < b);
    memset(a, 7, 4000);
    /* a cannot grow: b is right after it. */
    CHECK(!au_region_resize(&r, a, 5000) && au_region_size(a) >= 4000 && au_region_size(a) < 5000);
    /* b grows into the free space after it, in place. */
    memset(b, 9, 4000);
    CHECK(au_region_resize(&r, b, 20000) && au_region_size(b) >= 20000);
    bool same = true;
    for (int i = 0; i < 4000; i++)
        same = same && b[i] == 9;
    CHECK(same);
    /* b shrinks: the tail goes back and joins the rest. */
    CHECK(au_region_resize(&r, b, 1000) && au_region_size(b) < 1100);
    size_t f, big;
    au_region_free_space(&r, &f, &big);
    CHECK(f == big && r.blocks == 2);
    /* With b freed, a grows into the space b left. */
    au_region_free(&r, b);
    CHECK(au_region_resize(&r, a, 30000) && au_region_size(a) >= 30000);
    same = true;
    for (int i = 0; i < 4000; i++)
        same = same && a[i] == 7;
    CHECK(same);
    /* Not past the chunk's end. */
    CHECK(!au_region_resize(&r, a, CH));
    au_region_free(&r, a);
    au_region_free_space(&r, &f, &big);
    CHECK(f == free0 && big == free0 && r.blocks == 0 && r.used == 0);
}

/* The pool's large blocks grow and shrink in place when the platform's
 * resize allows it: here a region stands in for the Mac's heap. */
static AuRegion big_region;
static size_t big_mem[(1u << 20) / sizeof(size_t)];

static void *big_get(size_t n, unsigned how)
{
    void *p = (how & AU_HEAP_GET_SLAB) ? NULL : au_region_alloc(&big_region, n, false);
    return p ? p : get(n, how);
}

static void big_release(void *p)
{
    if (au_region_owns(&big_region, p))
        au_region_free(&big_region, p);
    else
        release(p);
}

static bool big_resize(void *p, size_t n)
{
    return au_region_owns(&big_region, p) && au_region_resize(&big_region, p, n);
}

static void resize_in_place(void)
{
    au_region_init(&big_region);
    CHECK(au_region_add(&big_region, big_mem, sizeof big_mem));
    au_heap_enable(big_get, big_release);
    au_heap_set_resize(big_resize);
    size_t resized0 = stats().resized;
    unsigned char *p = au_alloc(20000);
    for (int i = 0; i < 20000; i++)
        p[i] = (unsigned char)i;
    unsigned char *q = au_realloc(p, 200000);
    CHECK(q == p && stats().resized == resized0 + 1);
    bool same = true;
    for (int i = 0; i < 20000; i++)
        same = same && q[i] == (unsigned char)i;
    CHECK(same);
    /* Another block after it: the next growth must move. */
    void *blocker = au_alloc(30000);
    unsigned char *r2 = au_realloc(q, 400000);
    CHECK(r2 != q);
    same = true;
    for (int i = 0; i < 20000; i++)
        same = same && r2[i] == (unsigned char)i;
    CHECK(same);
    /* Shrinking stays in place and gives the tail back. */
    unsigned char *s2 = au_realloc(r2, 17000);
    CHECK(s2 == r2 && stats().resized == resized0 + 2);
    au_free(s2);
    au_free(blocker);
    CHECK(big_region.blocks == 0);
    au_heap_set_resize(NULL);
    au_heap_enable(get, release);
}

static int handler_calls;
static void *cache_block; /* what the release handler can free */

static bool handler(size_t wanted)
{
    (void)wanted;
    handler_calls++;
    if (!cache_block)
        return false;
    au_free(cache_block);
    cache_block = NULL;
    return true;
}

/* When the platform has no memory, the empty slabs go back first; the
 * application's release handler runs only when that is not enough. */
static void low_memory(void)
{
    au_heap_trim();
    au_set_release_handler(handler);
    enum { L = 100000 };

    /* Kept empty slabs, and a budget that fits the block only without
     * them. */
    void *s = au_alloc(4112), *t = au_alloc(100);
    au_free(s);
    au_free(t);
    size_t kept = stats().empty_bytes;
    CHECK(kept > 0);
    sys_budget = sys_bytes - kept + L + 64;
    void *p = au_try_alloc(L);
    CHECK(p != NULL);
    CHECK(stats().empty_bytes == 0);
    au_free(p);
    CHECK(handler_calls == 0);

    /* The same through au_alloc: the handler is not called. */
    s = au_alloc(4112);
    au_free(s);
    kept = stats().empty_bytes;
    sys_budget = sys_bytes - kept + L + 64;
    p = au_alloc(L);
    CHECK(handler_calls == 0 && stats().empty_bytes == 0);
    au_free(p);

    /* Not enough: the handler frees a cache, and the slab it emptied goes
     * back too. */
    sys_budget = (size_t)-1;
    cache_block = au_alloc(8000);
    sys_budget = sys_bytes + L / 2;
    p = au_alloc(L / 2 + 8000);
    CHECK(p != NULL);
    CHECK(handler_calls == 1 && cache_block == NULL);
    au_free(p);
    sys_budget = (size_t)-1;
    au_set_release_handler(NULL);
}

int main(void)
{
    CHECK(!au_heap_enabled());
    au_heap_enable(get, release);
    CHECK(au_heap_enabled());

    /* Through au_alloc: the public functions use the pool. */
    char *s = au_strndup("hello", 5);
    CHECK(sys_blocks == 1 && !strcmp(s, "hello"));
    au_free(s);

    random_test();
    mid_classes();
    arenas();
    keep_limit();
    low_memory();
    zones();
    region();
    region_resize();
    resize_in_place();

    /* The same class keeps a block in place when it grows a little. */
    char *p = au_alloc(130);
    char *q = au_realloc(p, 150);
    CHECK(p == q);
    au_free(q);
    au_free(NULL);

    /* A large block shrinking into the slabs' range moves there. */
    unsigned char *big = au_alloc(20000);
    for (int i = 0; i < 20000; i++)
        big[i] = (unsigned char)i;
    big = au_realloc(big, 10001);
    bool same = true;
    for (int i = 0; i < 10001; i++)
        same = same && big[i] == (unsigned char)i;
    CHECK(same && stats().large == 0 && stats().mid_blocks == 1);
    au_free(big);

    /* No platform memory: the non-fatal functions return NULL. */
    sys_fail_after = 0;
    CHECK(au_try_alloc(100000) == NULL);
    /* Blocks still come from the empty slabs held, then fail. */
    void *held[16];
    for (int i = 0; i < 16; i++)
        held[i] = au_try_alloc(2000);
    void *more = NULL;
    for (int i = 0; i < 1000 && (more = au_try_alloc(2048)) != NULL; i++)
        ;
    CHECK(more == NULL);
    CHECK(au_try_alloc(4112) == NULL);
    sys_fail_after = (size_t)-1;
    for (int i = 0; i < 16; i++)
        au_free(held[i]);

    printf(fails ? "heap: %d failures\n" : "heap: ok\n", fails);
    return fails ? 1 : 0;
}
