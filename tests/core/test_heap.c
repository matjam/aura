/* The pooled allocator (src/core/heap.c) over malloc: random allocations,
 * reallocations and frees with every block's contents checked, alignment,
 * the statistics, and that everything goes back to the platform. */
#include "core/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t sys_blocks, sys_fail_after = (size_t)-1;

static void *get(size_t n)
{
    if (sys_fail_after == 0)
        return NULL;
    if (sys_fail_after != (size_t)-1)
        sys_fail_after--;
    sys_blocks++;
    return malloc(n);
}

static void release(void *p)
{
    sys_blocks--;
    free(p);
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

static size_t pick_size(void)
{
    uint32_t r = rnd() % 100;
    if (r < 60)
        return 1 + rnd() % 130; /* au_alloc(0) allocates one byte */
    if (r < 90)
        return 1 + rnd() % 2100;
    if (r < 98)
        return 2000 + rnd() % 6000;
    return 30000 + rnd() % 200000;
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

    enum { N = 4000 };
    static Blk b[N];
    for (int round = 0; round < 200000; round++) {
        Blk *k = &b[rnd() % N];
        uint32_t op = rnd() % 10;
        if (!k->p) {
            k->n = pick_size();
            k->seed = (unsigned char)rnd();
            k->p = au_alloc(k->n);
            CHECK(((uintptr_t)k->p & (2 * sizeof(void *) - 1)) == 0);
            fill(k, 0);
        } else if (op < 4) {
            CHECK(intact(k, k->n));
            au_free(k->p);
            k->p = NULL;
        } else if (op < 7) {
            size_t n = pick_size(), keep = n < k->n ? n : k->n;
            k->p = au_realloc(k->p, n);
            CHECK(intact(k, keep));
            k->n = n;
            fill(k, keep);
        } else {
            CHECK(intact(k, k->n));
        }
        if (fails > 10)
            return 1;
    }
    AuHeapStats st;
    au_heap_stats(&st);
    size_t live = 0, bytes = 0;
    for (int i = 0; i < N; i++)
        if (b[i].p) {
            live++;
            bytes += b[i].n;
        }
    CHECK(st.blocks == live);
    CHECK(st.used_bytes == bytes);
    CHECK(st.slabs + st.large == sys_blocks);
    for (int i = 0; i < N; i++)
        if (b[i].p) {
            CHECK(intact(&b[i], b[i].n));
            au_free(b[i].p);
        }
    au_heap_stats(&st);
    CHECK(st.blocks == 0 && st.used_bytes == 0 && st.large == 0);
    /* Only the pool of empty slabs is still held. */
    CHECK(st.slabs <= 8 && sys_blocks == st.slabs);

    /* The same class keeps a block in place when it grows a little. */
    char *p = au_alloc(130);
    char *q = au_realloc(p, 150);
    CHECK(p == q);
    au_free(q);
    au_free(NULL);

    /* No platform memory: the non-fatal functions return NULL. */
    void *held[16];
    for (int i = 0; i < 16; i++)
        held[i] = NULL;
    sys_fail_after = 0;
    CHECK(au_try_alloc(100000) == NULL);
    /* Small blocks still come from the empty slabs held, then fail. */
    int got = 0;
    for (int i = 0; i < 16; i++) {
        held[i] = au_try_alloc(2000);
        if (held[i])
            got++;
    }
    (void)got;
    void *more = NULL;
    for (int i = 0; i < 1000 && (more = au_try_alloc(2048)) != NULL; i++)
        ;
    CHECK(more == NULL);
    sys_fail_after = (size_t)-1;

    printf(fails ? "heap: %d failures\n" : "heap: ok\n", fails);
    return fails ? 1 : 0;
}
