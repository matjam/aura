/* memcpy and memmove for the Mac OS 9 build, in place of newlib's.
 *
 * newlib's copies move words only when the source and the destination
 * are both 4-byte aligned, and memmove copies byte by byte whenever it
 * has to copy backwards (the regions overlap with the destination
 * higher). Strings, text and the script engine's byte arrays are rarely
 * aligned alike. These copy words in both cases: when the two pointers
 * differ in alignment, each destination word is built from two aligned
 * source words with shifts (PowerPC is big-endian), so no load or store
 * is misaligned. No dcbz: it faults on memory that is not cacheable,
 * such as video memory.
 *
 * The functions are named by FM_NAME so that tests/core/test_fastmem.c
 * can build this file on the host under other names and compare it with
 * the C library. Built with -fno-builtin and
 * -fno-tree-loop-distribute-patterns (CMakeLists.txt), so that the
 * compiler does not turn these loops back into calls to themselves. */
#include <stddef.h>
#include <stdint.h>

#ifndef FM_NAME
#define FM_NAME(x) x
#endif

#if !defined(FM_BIG_ENDIAN) && defined(__BIG_ENDIAN__)
#define FM_BIG_ENDIAN 1
#endif

/* Word access that the compiler may not assume about type aliasing. */
typedef uint32_t __attribute__((__may_alias__)) fm_word;

/* Joins the tail of a (from byte off) with the head of b, as the bytes
 * would lie in memory. */
#ifdef FM_BIG_ENDIAN
#define MERGE(a, b, ls, rs) (((a) << (ls)) | ((b) >> (rs)))
#else
#define MERGE(a, b, ls, rs) (((a) >> (ls)) | ((b) << (rs)))
#endif

#define SMALL 16

void *FM_NAME(memcpy)(void *restrict dst, const void *restrict src, size_t n);
void *FM_NAME(memmove)(void *dst, const void *src, size_t n);

/* Forward copy. Safe for overlap when d is below s: every word is read
 * before the stores reach it. */
static void copy_fwd(unsigned char *d, const unsigned char *s, size_t n)
{
    if (n >= SMALL) {
        while ((uintptr_t)d & 3) {
            *d++ = *s++;
            n--;
        }
        unsigned off = (unsigned)((uintptr_t)s & 3);
        fm_word *dw = (fm_word *)d;
        size_t words = n >> 2;
        if (!off) {
            const fm_word *sw = (const fm_word *)s;
            for (; words >= 4; words -= 4) {
                fm_word a = sw[0], b = sw[1], c = sw[2], e = sw[3];
                dw[0] = a;
                dw[1] = b;
                dw[2] = c;
                dw[3] = e;
                sw += 4;
                dw += 4;
            }
            while (words--)
                *dw++ = *sw++;
            s = (const unsigned char *)sw;
        } else {
            unsigned ls = off * 8, rs = 32 - ls;
            const fm_word *sw = (const fm_word *)(s - off);
            fm_word a = *sw++;
            /* Each step reads the next aligned source word, which holds
             * bytes of the copy (n - 4k >= 4 and off > 0), so no read
             * goes past the source. */
            for (; words >= 4; words -= 4) {
                fm_word b = sw[0], c = sw[1], e = sw[2], f = sw[3];
                dw[0] = MERGE(a, b, ls, rs);
                dw[1] = MERGE(b, c, ls, rs);
                dw[2] = MERGE(c, e, ls, rs);
                dw[3] = MERGE(e, f, ls, rs);
                a = f;
                sw += 4;
                dw += 4;
            }
            while (words--) {
                fm_word b = *sw++;
                *dw++ = MERGE(a, b, ls, rs);
                a = b;
            }
            s = (const unsigned char *)sw - 4 + off;
        }
        d = (unsigned char *)dw;
        n &= 3;
    }
    while (n--)
        *d++ = *s++;
}

/* Backward copy, for d above s: from the end down. */
static void copy_bwd(unsigned char *d, const unsigned char *s, size_t n)
{
    d += n;
    s += n;
    if (n >= SMALL) {
        while ((uintptr_t)d & 3) {
            *--d = *--s;
            n--;
        }
        unsigned off = (unsigned)((uintptr_t)s & 3);
        fm_word *dw = (fm_word *)d;
        size_t words = n >> 2;
        if (!off) {
            const fm_word *sw = (const fm_word *)s;
            for (; words >= 4; words -= 4) {
                fm_word a = sw[-1], b = sw[-2], c = sw[-3], e = sw[-4];
                dw[-1] = a;
                dw[-2] = b;
                dw[-3] = c;
                dw[-4] = e;
                sw -= 4;
                dw -= 4;
            }
            while (words--)
                *--dw = *--sw;
            s = (const unsigned char *)sw;
        } else {
            /* As forward: the destination word is a lower aligned
             * source word from byte off, then the next word's first off
             * bytes. */
            unsigned ls = off * 8, rs = 32 - ls;
            /* The aligned word holding the byte at s - 1 and those
             * below; b is the higher word of each pair. */
            const fm_word *sw = (const fm_word *)(s - off);
            fm_word b = *sw;
            while (words--) {
                fm_word a = *--sw;
                *--dw = MERGE(a, b, ls, rs);
                b = a;
            }
            s = (const unsigned char *)sw + off;
        }
        d = (unsigned char *)dw;
        n &= 3;
    }
    while (n--)
        *--d = *--s;
}

void *FM_NAME(memcpy)(void *restrict dst, const void *restrict src, size_t n)
{
    copy_fwd(dst, src, n);
    return dst;
}

void *FM_NAME(memmove)(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || !n)
        return dst;
    if ((uintptr_t)d - (uintptr_t)s >= n)
        copy_fwd(d, s, n); /* d below s, or no overlap */
    else
        copy_bwd(d, s, n);
    return dst;
}
