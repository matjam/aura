/* Fuzzes the cookie store: mutated Set-Cookie values from a seed corpus
 * against several request contexts, header generation, and the cookie
 * file parser (with a serialize/parse round trip that must keep every
 * persistent cookie).
 *
 *   fuzz_cookies ITERATIONS SEED */
#include "net/cookies.h"
#include "url/url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_state;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static const char *seeds[] = {
    "SID=31d4d96e407aad42; Path=/; Secure; HttpOnly",
    "lang=en-US; Path=/; Domain=example.com",
    "a=b; Expires=Wed, 09 Jun 2027 10:18:14 GMT",
    "a=b; Max-Age=60; SameSite=Strict",
    "__Host-x=1; Secure; Path=/",
    "__Secure-y=2; Secure; Domain=.example.com",
    "p=1; Partitioned; Secure; SameSite=None; Path=/",
    "q=\"x y\"; expires=31-Dec-99 23:59:59; path=/a/b",
    "noequals",
    "=v",
    "a=b;;;; ; =;Domain=;Path=;Max-Age=-1",
    "x=1; Domain=com",
    "x=1; Domain=.co.uk; SameSite=Lax",
};

static const char *urls[] = {"https://www.example.com/a/b", "http://example.com/", "https://sub.example.co.uk/",
                             "https://1.2.3.4/x", "https://[::1]/", "http://localhost/p"};

static const char dict[] = "=;, \t\"%\x01\x7f\xff/.-_SameSiteNoneLaxStrictSecureHttpOnlyPartitionedDomainPath"
                           "MaxAgeExpires0123456789GMT";

static size_t mutate(char *buf, size_t len, size_t cap)
{
    int n = 1 + (int)(rnd() % 6);
    for (int k = 0; k < n; k++) {
        uint32_t op = rnd() % 5;
        size_t pos = len ? rnd() % (len + 1) : 0;
        if (op == 0 && len) { /* flip */
            buf[rnd() % len] ^= (char)(1u << (rnd() % 8));
        } else if (op == 1 && len < cap) { /* insert dictionary byte */
            memmove(buf + pos + 1, buf + pos, len - pos);
            buf[pos] = dict[rnd() % (sizeof dict - 1)];
            len++;
        } else if (op == 2 && len) { /* delete */
            if (pos == len)
                pos--;
            memmove(buf + pos, buf + pos + 1, len - pos - 1);
            len--;
        } else if (op == 3 && len < cap) { /* splice from another seed */
            const char *s = seeds[rnd() % (sizeof seeds / sizeof *seeds)];
            size_t sl = strlen(s), take = rnd() % (sl + 1);
            if (take > cap - len)
                take = cap - len;
            memmove(buf + pos + take, buf + pos, len - pos);
            memcpy(buf + pos, s, take);
            len += take;
        } else if (op == 4 && len < cap) { /* repeat a chunk (long values) */
            size_t rep = rnd() % 64;
            while (rep-- && len < cap) {
                memmove(buf + pos + 1, buf + pos, len - pos);
                buf[pos] = 'A';
                len++;
            }
        }
    }
    return len;
}

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 10000;
    rng_state = argc > 2 ? (uint32_t)atol(argv[2]) : 1;
    if (!rng_state)
        rng_state = 1;
    AuUrl u[6];
    for (int i = 0; i < 6; i++)
        if (!au_url_parse(&u[i], urls[i], NULL))
            return 2;
    AuCookieJar *jar = au_cookie_jar_new();
    enum { CAP = 6000 };
    char *buf = malloc(CAP + 1);
    long stored = 0;
    AuBuf out;
    au_buf_init(&out);
    for (long it = 0; it < iters; it++) {
        const char *s = seeds[rnd() % (sizeof seeds / sizeof *seeds)];
        size_t len = strlen(s);
        memcpy(buf, s, len);
        len = mutate(buf, len, CAP);
        buf[len] = 0;
        AuCookieRequest rq;
        memset(&rq, 0, sizeof rq);
        rq.url = &u[rnd() % 6];
        rq.site_for_cookies = (rnd() & 1) ? &u[rnd() % 6] : NULL;
        rq.initiator = (rnd() & 1) ? &u[rnd() % 6] : NULL;
        rq.top_level_nav = !rq.site_for_cookies;
        rq.safe_method = rnd() & 1;
        rq.now = 1800000000LL + (int64_t)(it / 16);
        uint32_t pol = rnd() % 8;
        au_cookie_jar_set_policy(jar, pol == 0 ? AU_COOKIE_POLICY_NONE : AU_COOKIE_POLICY_ALL, pol & 1);
        if (rnd() & 1)
            stored += au_cookie_jar_set(jar, &rq, buf, NULL);
        else
            au_cookie_jar_document_set(jar, &rq, buf);
        au_buf_clear(&out);
        au_cookie_jar_header(jar, &rq, &out);
        au_buf_clear(&out);
        au_cookie_jar_document_get(jar, &rq, &out);

        if (it % 1000 == 999) {
            /* Round trip: everything persistent comes back. */
            AuBuf file;
            au_buf_init(&file);
            au_cookie_jar_serialize(jar, &file, rq.now);
            AuCookieJar *j2 = au_cookie_jar_new();
            if (!au_cookie_jar_parse(j2, file.data ? file.data : "", file.len, rq.now)) {
                printf("round trip failed to parse at %ld\n", it);
                return 1;
            }
            AuBuf file2;
            au_buf_init(&file2);
            au_cookie_jar_serialize(j2, &file2, rq.now);
            if (file.len != file2.len || memcmp(file.data, file2.data, file.len)) {
                printf("round trip changed the file at %ld\n", it);
                return 1;
            }
            /* Mutated file. */
            size_t fl = file.len < CAP ? file.len : CAP;
            memcpy(buf, file.data, fl);
            fl = mutate(buf, fl, CAP);
            au_cookie_jar_parse(j2, buf, fl, rq.now);
            au_buf_free(&file);
            au_buf_free(&file2);
            au_cookie_jar_free(j2);
        }
        if (it % 5000 == 4999)
            au_cookie_jar_clear(jar);
    }
    printf("fuzz_cookies: %ld iterations, %ld stored, %zu in jar\n", iters, stored, au_cookie_jar_count(jar));
    au_buf_free(&out);
    au_cookie_jar_free(jar);
    for (int i = 0; i < 6; i++)
        au_url_free(&u[i]);
    free(buf);
    return 0;
}
