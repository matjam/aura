/* Script memory limits. See scriptmem.h. */
#include "scriptmem.h"

static uint32_t clamp_mb(uint32_t kb, uint32_t lo, uint32_t hi)
{
    if (kb < lo)
        kb = lo;
    if (kb > hi)
        kb = hi;
    return kb & ~(uint32_t)1023;
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
    if (kb <= 0)
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
        *page_kb = (uint32_t)au_script_memory_clamp(pref_page_kb);
    if (au_script_memory_clamp(pref_worker_kb))
        *worker_kb = (uint32_t)au_script_memory_clamp(pref_worker_kb);
}
