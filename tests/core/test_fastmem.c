/* The Mac OS 9 build's memcpy and memmove (src/platform/macos9/fastmem.c),
 * built here as fm_memcpy and fm_memmove: every length up to 300 at every
 * pair of source and destination alignments, and every overlap of memmove
 * both ways, against a byte-by-byte copy, with guard bytes around the
 * destination. The host is little-endian, so this checks the loops and
 * their bounds; the big-endian merge is its mirror and runs in the guest
 * (tools/bench.sh, tools/smoke.sh). */
#define FM_NAME(x) fm_##x
#include "../../src/platform/macos9/fastmem.c"

#include <stdio.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...) \
    do { \
        checks++; \
        if (!(cond)) { \
            if (failures++ < 20) { \
                printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__); \
                printf("\n"); \
            } \
        } \
    } while (0)

#define CAP 400

static void fill(unsigned char *p, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)(seed * 131u + i * 7u + (i >> 3));
}

static void test_copy(void)
{
    static unsigned char src[CAP + 16], dst[CAP + 32], want[CAP + 32];
    for (size_t n = 0; n <= 300; n++)
        for (unsigned so = 0; so < 8; so++)
            for (unsigned dof = 0; dof < 8; dof++) {
                fill(src, sizeof src, (unsigned)n);
                memset(dst, 0xA5, sizeof dst);
                memset(want, 0xA5, sizeof want);
                for (size_t i = 0; i < n; i++)
                    want[8 + dof + i] = src[so + i];
                void *r = fm_memcpy(dst + 8 + dof, src + so, n);
                CHECK(r == dst + 8 + dof, "memcpy returns its destination");
                CHECK(!memcmp(dst, want, sizeof dst), "memcpy n %zu src +%u dst +%u", n, so, dof);
                memset(dst, 0xA5, sizeof dst);
                fm_memmove(dst + 8 + dof, src + so, n);
                CHECK(!memcmp(dst, want, sizeof dst), "memmove apart n %zu src +%u dst +%u", n, so, dof);
            }
}

static void test_overlap(void)
{
    static unsigned char buf[CAP], want[CAP];
    for (size_t n = 0; n <= 200; n++)
        for (int delta = -40; delta <= 40; delta++)
            for (unsigned base = 0; base < 4; base++) {
                size_t s = 60 + base, d = (size_t)((int)s + delta);
                fill(buf, sizeof buf, (unsigned)(n + 3));
                memcpy(want, buf, sizeof buf);
                for (size_t i = 0; i < n; i++) {
                    /* What memmove means: the source as it was. */
                    static unsigned char tmp[CAP];
                    if (i == 0)
                        memcpy(tmp, buf + s, n);
                    want[d + i] = tmp[i];
                }
                void *r = fm_memmove(buf + d, buf + s, n);
                CHECK(r == buf + d, "memmove returns its destination");
                CHECK(!memcmp(buf, want, sizeof buf), "memmove overlap n %zu delta %d base %u", n, delta, base);
            }
}

int main(void)
{
    test_copy();
    test_overlap();
    printf("fastmem: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
