/* The TLS library's platform hooks. See tls_platform.h. */
#include "tls_platform.h"

#include "core/hash.h"

#include "mbedtls/platform.h"
#include "mbedtls/platform_time.h"
#include "psa/crypto.h"

#include <string.h>
#include <time.h>

/* ---- Memory ----
 *
 * The library allocates through these. A failed allocation returns NULL
 * and the library fails the operation (the handshake ends with an error);
 * au_alloc's out-of-memory handler is never reached from here. Each block
 * carries its size so that the totals can be reported. */

typedef union Head {
    size_t size;
    uint64_t align64;
    void *alignp;
} Head;

static size_t g_live, g_peak;
static long g_fail_after = -1;

static void *tls_calloc(size_t n, size_t size)
{
    if (size && n > ((size_t)-1 - sizeof(Head)) / size)
        return NULL;
    if (g_fail_after == 0)
        return NULL;
    if (g_fail_after > 0)
        g_fail_after--;
    size_t bytes = n * size;
    Head *h = au_try_alloc(sizeof *h + bytes);
    if (!h)
        return NULL;
    h->size = bytes;
    memset(h + 1, 0, bytes);
    g_live += bytes;
    if (g_live > g_peak)
        g_peak = g_live;
    return h + 1;
}

static void tls_free(void *p)
{
    if (!p)
        return;
    Head *h = (Head *)p - 1;
    g_live -= h->size;
    au_free(h);
}

void au_tls_memory(size_t *live, size_t *peak)
{
    if (live)
        *live = g_live;
    if (peak)
        *peak = g_peak;
}

void au_tls_fail_alloc_after(long n) { g_fail_after = n; }

/* ---- Clocks ---- */

static int64_t g_wall;
static uint32_t g_last_ms;
static int64_t g_ms;

void au_tls_set_wall(int64_t now) { g_wall = now; }

void au_tls_tick(uint32_t now_ms)
{
    g_ms += (uint32_t)(now_ms - g_last_ms); /* the platform's counter wraps */
    g_last_ms = now_ms;
}

static mbedtls_time_t tls_time(mbedtls_time_t *out)
{
    mbedtls_time_t t = (mbedtls_time_t)g_wall;
    if (out)
        *out = t;
    return t;
}

/* MBEDTLS_PLATFORM_MS_TIME_ALT */
mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)g_ms; }

/* MBEDTLS_PLATFORM_GMTIME_R_ALT: seconds since 1970 to a calendar date,
 * without the C library's time zone code (days-to-civil conversion). */
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf)
{
    int64_t t = (int64_t)*tt, days = t / 86400, rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    memset(tm_buf, 0, sizeof *tm_buf);
    tm_buf->tm_year = (int)((int64_t)yoe + era * 400 + (m <= 2)) - 1900;
    tm_buf->tm_mon = (int)m - 1;
    tm_buf->tm_mday = (int)d;
    tm_buf->tm_hour = (int)(rem / 3600);
    tm_buf->tm_min = (int)(rem % 3600 / 60);
    tm_buf->tm_sec = (int)(rem % 60);
    return tm_buf;
}

/* ---- Entropy ----
 *
 * The pool is a SHA-256 chain over everything fed to it. The library's
 * random generator (HMAC-DRBG) seeds and reseeds itself by calling
 * mbedtls_platform_get_entropy, which derives its output from the pool
 * and then replaces the pool's state, so no output can be derived
 * again. The entropy itself comes from the platform (on Mac OS 9, the
 * pool in src/platform/macos9/entropy.c), which gives nothing until it
 * has credited 128 bits. */

static uint8_t g_pool[32];
static bool g_pool_fed, g_started, g_hooks;

static void pool_hash(const char *label, const uint8_t *extra, size_t extra_len, uint8_t out[32])
{
    AuSha256 c;
    au_sha256_init(&c);
    au_sha256_update(&c, label, strlen(label));
    au_sha256_update(&c, g_pool, sizeof g_pool);
    au_sha256_update(&c, extra, extra_len);
    au_sha256_out(&c, out);
}

/* MBEDTLS_PSA_DRIVER_GET_ENTROPY */
int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags, size_t *estimate_bits, unsigned char *output,
                                 size_t output_size)
{
    if (flags != 0)
        return PSA_ERROR_NOT_SUPPORTED;
    if (!g_pool_fed)
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    uint8_t block[32], ctr[4] = {0, 0, 0, 0};
    for (size_t done = 0; done < output_size;) {
        pool_hash("out", ctr, sizeof ctr, block);
        size_t n = output_size - done < sizeof block ? output_size - done : sizeof block;
        memcpy(output + done, block, n);
        done += n;
        for (int i = 3; i >= 0 && ++ctr[i] == 0; i--) {
        }
    }
    pool_hash("next", NULL, 0, block);
    memcpy(g_pool, block, sizeof g_pool);
    memset(block, 0, sizeof block);
    *estimate_bits = 8 * output_size;
    return 0;
}

void au_tls_platform_init(void)
{
    if (g_hooks)
        return;
    mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
    mbedtls_platform_set_time(tls_time);
    g_hooks = true;
}

bool au_tls_entropy_feed(const uint8_t *seed, size_t len)
{
    uint8_t next[32];
    pool_hash("feed", seed, len, next);
    memcpy(g_pool, next, sizeof g_pool);
    memset(next, 0, sizeof next);
    g_pool_fed = true;
    if (!g_started) {
        au_tls_platform_init();
        /* Seeds the library's generator from the pool. */
        if (psa_crypto_init() != PSA_SUCCESS)
            return false;
        g_started = true;
    }
    return true;
}

bool au_tls_entropy_ready(void) { return g_started; }

bool au_tls_random(uint8_t *out, size_t len)
{
    return g_started && psa_generate_random(out, len) == PSA_SUCCESS;
}
