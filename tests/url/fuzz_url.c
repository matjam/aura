/* Deterministic URL parser fuzzer: mutates seed URLs (and random bytes),
 * parses each against a base and without one, and checks that the
 * serialization reparses to itself (the parser is idempotent on its own
 * output). Under ASan/UBSan it finds memory errors.
 *
 * usage: fuzz_url ITERATIONS SEED */
#include "../fuzz.h"
#include "url/url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_s;
static uint32_t rnd(void)
{
    uint32_t x = rng_s ? rng_s : 0x12345678u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_s = x;
}

static const char *seeds[] = {
    "http://user:pw@example.com:8080/a/b/../c?q=1&r=%41#frag",
    "https://[::ffff:1.2.3.4]:443/x",
    "file:///C|/dir/../file.txt",
    "file://host/share/x",
    "data:text/html;base64,PGI+aGk8L2I+",
    "about:blank#top",
    "//other.example/p?q",
    "../../x/./y%2e%2E/z",
    "http://0x7F.0.0.01/",
    "http://xn--bcher-kva.example/",
    "http://bücher.example/ü?ü#ü",
    "non-special://h%41st:99/p/../q",
    "web+demo:/.//x",
    "\\\\server\\share",
    "http://a:b@c@d/",
    "?just=query",
    "#justfrag",
    "javascript:alert(1)",
    "http://[1:2:3:4:5:6:7:8]/",
    "ftp://ftp.example.org:21/pub",
};

static const char alphabet[] = "/\\?#@:[]%.0123456789abcdefxXAZ%2e%2E ~|^<>\"'`{}\t\n";

static void check(const char *in, size_t n, const AuUrl *base)
{
    AuUrl u;
    if (!au_url_parse_n(&u, in, n, base))
        return;
    AuUrl v;
    if (!au_url_parse(&v, u.href, NULL)) {
        /* A URL whose serialization does not reparse: only allowed for
         * the known non-idempotent cases (none for our inputs). */
        printf("fuzz_url: serialization does not reparse: %s\n", u.href);
        abort();
    }
    if (strcmp(u.href, v.href)) {
        printf("fuzz_url: not idempotent: %s -> %s\n", u.href, v.href);
        abort();
    }
    AuOrigin o;
    au_url_origin(&u, &o);
    au_origin_free(&o);
    au_url_free(&v);
    au_url_free(&u);
}

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 10000;
    rng_s = (uint32_t)(argc > 2 ? atol(argv[2]) : 1) * 2654435761u + 3;
    size_t nseeds = sizeof seeds / sizeof seeds[0];
    AuUrl bases[3];
    au_url_parse(&bases[0], "http://example.org/foo/bar?x#y", NULL);
    au_url_parse(&bases[1], "file:///C:/dir/file", NULL);
    au_url_parse(&bases[2], "sc://h/a/b", NULL);
    char buf[512];
    long notidem = 0;
    for (long it = 0; it < iters; it++) {
        if (fz_capped()) { /* AURA_FUZZ_SECONDS */
            iters = it;
            break;
        }
        size_t n;
        if (rnd() % 8) {
            const char *s = seeds[rnd() % nseeds];
            n = strlen(s);
            memcpy(buf, s, n);
            int muts = 1 + (int)(rnd() % 5);
            for (int m = 0; m < muts; m++) {
                uint32_t k = rnd();
                size_t at = n ? rnd() % (n + 1) : 0;
                char ch = (k & 16) ? alphabet[(k >> 8) % (sizeof alphabet - 1)] : (char)(k >> 8);
                switch (k % 4) {
                case 0:
                    if (at < n)
                        buf[at] = ch;
                    break;
                case 1:
                    if (n < sizeof buf - 1) {
                        memmove(buf + at + 1, buf + at, n - at);
                        buf[at] = ch;
                        n++;
                    }
                    break;
                case 2:
                    if (at < n) {
                        memmove(buf + at, buf + at + 1, n - at - 1);
                        n--;
                    }
                    break;
                default: { /* splice another seed */
                    const char *t = seeds[rnd() % nseeds];
                    size_t tn = strlen(t), from = rnd() % (tn + 1), len = tn - from;
                    if (at + len > sizeof buf - 1)
                        len = sizeof buf - 1 - at;
                    memcpy(buf + at, t + from, len);
                    if (at + len > n)
                        n = at + len;
                }
                }
            }
        } else {
            n = rnd() % 64;
            for (size_t i = 0; i < n; i++)
                buf[i] = (char)rnd();
        }
        /* The API takes NUL-free strings with au_url_parse; parse_n gets
         * the buffer as is. */
        check(buf, n, NULL);
        check(buf, n, &bases[rnd() % 3]);
    }
    (void)notidem;
    for (int i = 0; i < 3; i++)
        au_url_free(&bases[i]);
    printf("fuzz_url: %ld iterations, no faults\n", iters);
    return 0;
}
