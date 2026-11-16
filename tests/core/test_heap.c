/* The pooled allocator (src/core/heap.c) over malloc: random allocations,
 * reallocations and frees with every block's contents checked, alignment,
 * the statistics, the slabs of blocks over 2 KB (reuse, one empty slab
 * kept per class, a live block pinning one slab), au_heap_trim and the
 * release path, and that everything goes back to the platform.
 *
 * Nothing in the allocator depends on byte order; reallocations of odd
 * lengths check that the word-at-a-time copy also moves the last bytes. */
#include "core/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The platform: malloc, with a count of blocks and bytes, a failure after
 * N more calls, and a budget of bytes it may hand out. */
static size_t sys_blocks, sys_bytes, sys_fail_after = (size_t)-1, sys_budget = (size_t)-1;

typedef struct SysHdr {
    size_t n, pad; /* two words: the block after it keeps malloc's alignment for the pool */
} SysHdr;

static void *get(size_t n)
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
    sys_blocks++;
    sys_bytes += n;
    return h + 1;
}

static void release(void *p)
{
    SysHdr *h = (SysHdr *)p - 1;
    sys_blocks--;
    sys_bytes -= h->n;
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
 * 16 to a slab, a freed block reused, one empty slab of the class kept. */
static void arenas(void)
{
    au_heap_trim();
    size_t blocks0 = sys_blocks;
    enum { NA = 150, PER = 16 };
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
