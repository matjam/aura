/* Sanitizer builds only: stops on a null pointer passed to memcpy, memmove,
 * memcmp or memset, even with length 0.
 *
 * The C standard forbids that, and glibc declares these functions with
 * nonnull arguments, so UBSan on Linux reports it. Apple's headers do not,
 * so a sanitizer build on macOS said nothing and the Linux job in CI was
 * the first to notice. CMakeLists.txt force-includes this header into the
 * engine when the build uses -fsanitize, which makes the two agree. */
#ifndef AURA_CORE_NULLCHECK_H
#define AURA_CORE_NULLCHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void au_nullcheck_fail(const char *fn, const char *file, int line)
{
    fprintf(stderr, "%s:%d: null pointer passed to %s\n", file, line, fn);
    abort();
}

static inline void *au_nc_memcpy(void *d, const void *s, size_t n, const char *file, int line)
{
    if (!d || !s)
        au_nullcheck_fail("memcpy", file, line);
    return memcpy(d, s, n);
}

static inline void *au_nc_memmove(void *d, const void *s, size_t n, const char *file, int line)
{
    if (!d || !s)
        au_nullcheck_fail("memmove", file, line);
    return memmove(d, s, n);
}

static inline int au_nc_memcmp(const void *a, const void *b, size_t n, const char *file, int line)
{
    if (!a || !b)
        au_nullcheck_fail("memcmp", file, line);
    return memcmp(a, b, n);
}

static inline void *au_nc_memset(void *d, int c, size_t n, const char *file, int line)
{
    if (!d)
        au_nullcheck_fail("memset", file, line);
    return memset(d, c, n);
}

#undef memcpy
#undef memmove
#undef memcmp
#undef memset
#define memcpy(d, s, n) au_nc_memcpy((d), (s), (n), __FILE__, __LINE__)
#define memmove(d, s, n) au_nc_memmove((d), (s), (n), __FILE__, __LINE__)
#define memcmp(a, b, n) au_nc_memcmp((a), (b), (n), __FILE__, __LINE__)
#define memset(d, c, n) au_nc_memset((d), (c), (n), __FILE__, __LINE__)

#endif
