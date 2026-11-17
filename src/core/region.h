/* A first-fit allocator over memory the caller supplies (region.c).
 *
 * To place blocks yourself inside one or more chunks of memory, init an
 * AuRegion, add the chunks with au_region_add, and allocate with
 * au_region_alloc: a low block takes the lowest free space that fits, a
 * high block the highest. Freed blocks join their free neighbours.
 *
 * The Mac OS 9 application keeps the pooled allocator's long-lived
 * slabs and blocks in a region (src/platform/macos9/main.c); the host
 * tool uses one as a stand-in for the Mac's application heap, whose
 * NewPtr takes the lowest free space too (AURA_HEAP=mac,
 * src/platform/host/main.c).
 *
 * Every block has a header of two words: its size and the caller's word
 * (au_region_tag). Blocks are aligned to two words. Allocation and free
 * walk the list of free extents, which is kept in address order. */
#ifndef AURA_CORE_REGION_H
#define AURA_CORE_REGION_H

#include <stdbool.h>
#include <stddef.h>

#define AU_REGION_CHUNKS 32

typedef struct AuRegionFree AuRegionFree;

typedef struct AuRegion {
    struct {
        char *base;
        size_t bytes;
    } chunk[AU_REGION_CHUNKS];
    int nchunks;
    AuRegionFree *free;  /* free extents in address order */
    size_t total;        /* bytes of the chunks */
    size_t used;         /* bytes of the blocks in use, headers included */
    size_t blocks;       /* blocks in use */
} AuRegion;

void au_region_init(AuRegion *r);
/* Adds a chunk of memory; false when the region has AU_REGION_CHUNKS
 * already or the chunk is too small to hold a block. */
bool au_region_add(AuRegion *r, void *mem, size_t bytes);
/* NULL when no free extent fits n bytes. */
void *au_region_alloc(AuRegion *r, size_t n, bool high);
void au_region_free(AuRegion *r, void *p);
/* Makes block p hold n bytes without moving it: a shrink frees the tail,
 * a growth takes the free space right after the block. False when that
 * space is not free or too small (p is then unchanged). */
bool au_region_resize(AuRegion *r, void *p, size_t n);
/* Takes out a chunk that holds no block: returns its index, and the last
 * chunk moves to that index (the caller who keeps a table per chunk
 * moves its entry the same way); the chunk's memory is the caller's
 * again. -1 when every chunk holds a block. */
int au_region_drop_free_chunk(AuRegion *r);
/* Whether p is inside one of the region's chunks. */
bool au_region_owns(const AuRegion *r, const void *p);
/* The bytes a block can hold (n rounded up). */
size_t au_region_size(const void *p);
/* The caller's word of a block, 0 when it was allocated. */
size_t *au_region_tag(void *p);
/* Free bytes and the largest free extent's bytes (headers excluded). */
void au_region_free_space(const AuRegion *r, size_t *free_bytes, size_t *largest);
/* Calls fn for every block of every chunk in address order, free ones
 * included (p is the block's memory, n what it can hold). */
void au_region_walk(const AuRegion *r, void (*fn)(void *ctx, void *p, size_t n, bool is_free), void *ctx);

#endif
