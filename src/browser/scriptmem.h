/* Script memory limits: how much one page's script runtime, and one
 * worker's, may allocate.
 *
 * Each limit is a preference (content.script_memory_kb,
 * content.worker_memory_kb) holding 0 for Automatic or a number of KB.
 * Automatic follows the memory the machine has free when a page starts:
 *
 *   page   = half of what is free, at least 8 MB, at most 96 MB (192 MB
 *            with 64-bit pointers)
 *   worker = a quarter of what is free, at least 4 MB, at most 32 MB
 *            (64 MB with 64-bit pointers)
 *
 * "Free" is what the shell reports (AuBrowserHost.memory_free_kb): on
 * Mac OS 9 the free application heap plus free temporary memory, which
 * the allocator falls back to. A shell that reports nothing (the host
 * tools, tests) gets 64 MB and 32 MB (128 MB and 64 MB with 64-bit
 * pointers). Half, because a page's documents,
 * styles, layout and images need the rest, and several tabs share it;
 * the limits are caps, not reservations, and the engine also refuses
 * script memory when the machine itself runs short (au_memory_low).
 * The ceilings bound what one runaway page can take on a machine with
 * a lot of memory. A value the user types is clamped to 4 MB .. 256 MB.
 *
 * Why these numbers: a page with a widget in two frames and a worker
 * that all load one 860 KB script held 16 MB in its runtime and 6 MB in
 * the worker on the 64-bit host, with peaks of 19 MB and 8 MB, before
 * the widget ran; the old fixed 32 MB and 8 MB were reached. */
#ifndef AURA_BROWSER_SCRIPTMEM_H
#define AURA_BROWSER_SCRIPTMEM_H

#include <stdint.h>

#define AU_SCRIPT_MEM_CUSTOM_MIN_KB (4u * 1024)
#define AU_SCRIPT_MEM_CUSTOM_MAX_KB (256u * 1024)
/* The ceilings are twice as high with 64-bit pointers: the engine's
 * objects, values and arenas are close to twice the size, so the same
 * page needs about twice the memory. Turnstile's managed challenge peaks
 * at 112 MB on the 64-bit host (measured 2026-11-17) and stops at 96 MB. */
#define AU_SCRIPT_MEM_SCALE (sizeof(void *) >= 8 ? 2u : 1u)
#define AU_SCRIPT_MEM_PAGE_FLOOR_KB (8u * 1024)
#define AU_SCRIPT_MEM_PAGE_CEILING_KB (96u * 1024 * AU_SCRIPT_MEM_SCALE)
#define AU_SCRIPT_MEM_WORKER_FLOOR_KB (4u * 1024)
#define AU_SCRIPT_MEM_WORKER_CEILING_KB (32u * 1024 * AU_SCRIPT_MEM_SCALE)
/* Scaled too, so a page measured with the host tools meets the limit
 * where it would on a 32-bit Mac: GitHub's releases page filled 64 MB on
 * the host and the collector ran at nearly every allocation. */
#define AU_SCRIPT_MEM_PAGE_UNKNOWN_KB (64u * 1024 * AU_SCRIPT_MEM_SCALE)
#define AU_SCRIPT_MEM_WORKER_UNKNOWN_KB (32u * 1024 * AU_SCRIPT_MEM_SCALE)

/* What Automatic gives with free_kb of memory free (0: unknown), rounded
 * down to whole megabytes. */
void au_script_memory_auto(uint32_t free_kb, uint32_t *page_kb, uint32_t *worker_kb);
/* A preference value as stored: 0 stays 0 (Automatic), anything else is
 * clamped to the custom range. */
int au_script_memory_clamp(int kb);
/* The limits in force: the preference when it is not 0, else Automatic. */
void au_script_memory_limits(int pref_page_kb, int pref_worker_kb, uint32_t free_kb, uint32_t *page_kb,
                             uint32_t *worker_kb);

#endif
