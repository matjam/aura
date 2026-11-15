/* Shared helpers for the SVG tests. Header-only. */
#ifndef AURA_SVGTEST_H
#define AURA_SVGTEST_H

#include "svg/svg.h"
#include "image/image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int st_failures;

#define ST_CHECK(cond, ...)                                     \
    do {                                                        \
        if (!(cond)) {                                          \
            st_failures++;                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

typedef struct StRng {
    uint32_t s;
} StRng;
static inline uint32_t st_rng(StRng *r)
{
    r->s ^= r->s << 13;
    r->s ^= r->s >> 17;
    r->s ^= r->s << 5;
    return r->s;
}

static inline uint8_t *st_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
    if (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static inline void st_write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(data, 1, len, f);
    fclose(f);
}

#endif
