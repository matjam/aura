/* A first-fit allocator over memory the caller supplies (region.h).
 *
 * A chunk is tiled by blocks, each with a header of two words: the
 * block's bytes, header included, with bit 0 set while it is free, and
 * the caller's word. A free block also links to the next free one; the
 * free list is in address order, so a block being freed finds its
 * neighbours in one walk and joins them. Sizes are whole multiples of the
 * header, so the blocks after it stay aligned to two words. */
#include "region.h"

#include <stdint.h>
#include <string.h>

typedef struct RHdr {
    size_t size; /* bytes, header included; bit 0: free */
    size_t tag;
} RHdr;

struct AuRegionFree {
    RHdr h;
    AuRegionFree *next;
};

#define R_ALIGN (sizeof(RHdr))
#define R_MIN ((sizeof(AuRegionFree) + R_ALIGN - 1) & ~(R_ALIGN - 1))

static size_t bytes_of(const RHdr *h)
{
    return h->size & ~(size_t)1;
}

void au_region_init(AuRegion *r)
{
    memset(r, 0, sizeof *r);
}

/* The start of the chunk that holds p. Two chunks may be adjacent in
 * memory; a block never spans them. */
static const char *chunk_base(const AuRegion *r, const void *p)
{
    for (int i = 0; i < r->nchunks; i++)
        if ((const char *)p >= r->chunk[i].base && (const char *)p < r->chunk[i].base + r->chunk[i].bytes)
            return r->chunk[i].base;
    return NULL;
}

/* Puts the free block f into the list in address order, joined with the
 * free blocks right before and after it in its chunk. */
static void free_insert(AuRegion *r, AuRegionFree *f)
{
    AuRegionFree *prev = NULL, *next = r->free;
    while (next && (char *)next < (char *)f) {
        prev = next;
        next = next->next;
    }
    f->h.size |= 1;
    f->h.tag = 0;
    f->next = next;
    const char *base = chunk_base(r, f);
    if (next && (char *)f + bytes_of(&f->h) == (char *)next && chunk_base(r, next) == base) {
        f->h.size = (bytes_of(&f->h) + bytes_of(&next->h)) | 1;
        f->next = next->next;
    }
    if (prev && (char *)prev + bytes_of(&prev->h) == (char *)f && chunk_base(r, prev) == base) {
        prev->h.size = (bytes_of(&prev->h) + bytes_of(&f->h)) | 1;
        prev->next = f->next;
    } else if (prev) {
        prev->next = f;
    } else {
        r->free = f;
    }
}

bool au_region_add(AuRegion *r, void *mem, size_t bytes)
{
    if (r->nchunks == AU_REGION_CHUNKS)
        return false;
    uintptr_t a = (uintptr_t)mem;
    uintptr_t b = (a + R_ALIGN - 1) & ~(uintptr_t)(R_ALIGN - 1);
    if (bytes < (size_t)(b - a) + R_MIN)
        return false;
    bytes = (bytes - (size_t)(b - a)) & ~(R_ALIGN - 1);
    char *base = (char *)b;
    r->chunk[r->nchunks].base = base;
    r->chunk[r->nchunks].bytes = bytes;
    r->nchunks++;
    r->total += bytes;
    AuRegionFree *f = (AuRegionFree *)base;
    f->h.size = bytes;
    free_insert(r, f);
    return true;
}

void *au_region_alloc(AuRegion *r, size_t n, bool high)
{
    if (n > (size_t)-1 / 2)
        return NULL;
    size_t need = (sizeof(RHdr) + n + R_ALIGN - 1) & ~(R_ALIGN - 1);
    if (need < R_MIN)
        need = R_MIN;
    AuRegionFree *prev = NULL, *f = NULL, *fprev = NULL;
    for (AuRegionFree *e = r->free; e; prev = e, e = e->next)
        if (bytes_of(&e->h) >= need) {
            f = e;
            fprev = prev;
            if (!high)
                break;
        }
    if (!f)
        return NULL;
    size_t have = bytes_of(&f->h);
    RHdr *h;
    if (have - need < R_MIN) {
        /* the whole extent */
        if (fprev)
            fprev->next = f->next;
        else
            r->free = f->next;
        h = &f->h;
        need = have;
    } else if (high) {
        f->h.size = (have - need) | 1;
        h = (RHdr *)((char *)f + (have - need));
    } else {
        AuRegionFree *rest = (AuRegionFree *)((char *)f + need);
        rest->h.size = (have - need) | 1;
        rest->h.tag = 0;
        rest->next = f->next;
        if (fprev)
            fprev->next = rest;
        else
            r->free = rest;
        h = &f->h;
    }
    h->size = need;
    h->tag = 0;
    r->used += need;
    r->blocks++;
    return h + 1;
}

void au_region_free(AuRegion *r, void *p)
{
    if (!p)
        return;
    RHdr *h = (RHdr *)p - 1;
    r->used -= bytes_of(h);
    r->blocks--;
    free_insert(r, (AuRegionFree *)h);
}

int au_region_drop_free_chunk(AuRegion *r)
{
    AuRegionFree *prev = NULL;
    for (AuRegionFree *e = r->free; e; prev = e, e = e->next)
        for (int i = 0; i < r->nchunks; i++)
            if ((char *)e == r->chunk[i].base && bytes_of(&e->h) == r->chunk[i].bytes) {
                if (prev)
                    prev->next = e->next;
                else
                    r->free = e->next;
                r->total -= r->chunk[i].bytes;
                r->nchunks--;
                r->chunk[i] = r->chunk[r->nchunks];
                return i;
            }
    return -1;
}

bool au_region_owns(const AuRegion *r, const void *p)
{
    for (int i = 0; i < r->nchunks; i++)
        if ((const char *)p >= r->chunk[i].base && (const char *)p < r->chunk[i].base + r->chunk[i].bytes)
            return true;
    return false;
}

size_t au_region_size(const void *p)
{
    return bytes_of((const RHdr *)p - 1) - sizeof(RHdr);
}

size_t *au_region_tag(void *p)
{
    return &((RHdr *)p - 1)->tag;
}

void au_region_free_space(const AuRegion *r, size_t *free_bytes, size_t *largest)
{
    size_t sum = 0, big = 0;
    for (const AuRegionFree *e = r->free; e; e = e->next) {
        size_t b = bytes_of(&e->h) - sizeof(RHdr);
        sum += b;
        if (b > big)
            big = b;
    }
    if (free_bytes)
        *free_bytes = sum;
    if (largest)
        *largest = big;
}

void au_region_walk(const AuRegion *r, void (*fn)(void *ctx, void *p, size_t n, bool is_free), void *ctx)
{
    for (int i = 0; i < r->nchunks; i++) {
        char *p = r->chunk[i].base, *end = p + r->chunk[i].bytes;
        while (p < end) {
            RHdr *h = (RHdr *)p;
            size_t b = bytes_of(h);
            fn(ctx, h + 1, b - sizeof(RHdr), (h->size & 1) != 0);
            p += b;
        }
    }
}
