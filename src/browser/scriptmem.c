/* Script memory limits. See scriptmem.h. */
#include "scriptmem.h"

#include <stddef.h>

/* As AU_IMAGE_BUDGET_SCALE (src/image/cache.h), without its header. */
#define IMAGE_SCALE (sizeof(void *) >= 8 ? 4u : 1u)

static uint32_t clamp_mb(uint32_t kb, uint32_t lo, uint32_t hi)
{
    if (kb < lo)
        kb = lo;
    if (kb > hi)
        kb = hi;
    return kb & ~(uint32_t)1023;
}

/* A preference that is not Automatic, in KB (AU_MEM_UNLIMITED_KB for
 * Unlimited). */
static uint32_t pref_kb(int pref)
{
    int kb = au_script_memory_clamp(pref);
    return kb == AU_MEM_UNLIMITED ? AU_MEM_UNLIMITED_KB : (uint32_t)kb;
}

void au_script_memory_auto(uint32_t free_kb, uint32_t *page_kb, uint32_t *worker_kb)
{
    if (!free_kb) {
        *page_kb = AU_SCRIPT_MEM_PAGE_UNKNOWN_KB;
        *worker_kb = AU_SCRIPT_MEM_WORKER_UNKNOWN_KB;
        return;
    }
    *page_kb = clamp_mb(free_kb / 2, AU_SCRIPT_MEM_PAGE_FLOOR_KB, AU_SCRIPT_MEM_PAGE_CEILING_KB);
    *worker_kb = clamp_mb(free_kb / 4, AU_SCRIPT_MEM_WORKER_FLOOR_KB, AU_SCRIPT_MEM_WORKER_CEILING_KB);
}

int au_script_memory_clamp(int kb)
{
    if (kb < 0)
        return AU_MEM_UNLIMITED;
    if (kb == 0)
        return 0;
    if (kb < (int)AU_SCRIPT_MEM_CUSTOM_MIN_KB)
        return (int)AU_SCRIPT_MEM_CUSTOM_MIN_KB;
    if (kb > (int)AU_SCRIPT_MEM_CUSTOM_MAX_KB)
        return (int)AU_SCRIPT_MEM_CUSTOM_MAX_KB;
    return kb;
}

void au_script_memory_limits(int pref_page_kb, int pref_worker_kb, uint32_t free_kb, uint32_t *page_kb,
                             uint32_t *worker_kb)
{
    au_script_memory_auto(free_kb, page_kb, worker_kb);
    if (au_script_memory_clamp(pref_page_kb))
        *page_kb = pref_kb(pref_page_kb);
    if (au_script_memory_clamp(pref_worker_kb))
        *worker_kb = pref_kb(pref_worker_kb);
}

size_t au_memory_kb_bytes(uint32_t kb)
{
    /* Half the address space: limit plus a reserve cannot overflow. */
    if (kb == AU_MEM_UNLIMITED_KB || (size_t)kb > ((size_t)-1 / 2) / 1024)
        return (size_t)-1 / 2;
    return (size_t)kb * 1024;
}

uint32_t au_memory_bytes_kb(size_t bytes)
{
    if (bytes >= (size_t)-1 / 2)
        return AU_MEM_UNLIMITED_KB;
    size_t kb = bytes / 1024;
    return kb < (size_t)AU_MEM_UNLIMITED_KB ? (uint32_t)kb : AU_MEM_UNLIMITED_KB - 1;
}

uint32_t au_image_memory_auto(uint32_t free_kb)
{
    if (!free_kb)
        return AU_IMAGE_MEM_FLOOR_KB * IMAGE_SCALE;
    return clamp_mb(free_kb / 8, AU_IMAGE_MEM_FLOOR_KB, AU_IMAGE_MEM_CEILING_KB) * IMAGE_SCALE;
}

uint32_t au_image_memory_limit(int pref, uint32_t free_kb)
{
    return au_script_memory_clamp(pref) ? pref_kb(pref) : au_image_memory_auto(free_kb);
}
