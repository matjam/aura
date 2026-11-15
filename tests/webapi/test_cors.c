/* The CORS rules (src/net/cors.c) and the digests (src/net/digest.c). */
#include "net/cors.h"
#include "net/digest.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond)                                                                                               \
    do {                                                                                                          \
        checks++;                                                                                                 \
        if (!(cond)) {                                                                                            \
            failures++;                                                                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
        }                                                                                                         \
    } while (0)

static void hex(const uint8_t *d, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", d[i]);
}

int main(void)
{
    /* methods and request headers */
    CHECK(au_cors_forbidden_method("TRACE") && au_cors_forbidden_method("connect") && !au_cors_forbidden_method("PUT"));
    CHECK(au_cors_safelisted_method("GET") && au_cors_safelisted_method("POST") && !au_cors_safelisted_method("PUT"));
    CHECK(au_cors_forbidden_request_header("Cookie", "a") && au_cors_forbidden_request_header("sec-fetch-mode", "x") &&
          au_cors_forbidden_request_header("Proxy-Authorization", "x") && au_cors_forbidden_request_header("Host", "x"));
    CHECK(!au_cors_forbidden_request_header("X-Custom", "x") && !au_cors_forbidden_request_header("Authorization", "x"));
    CHECK(au_cors_forbidden_request_header("X-HTTP-Method-Override", "GET, trace") &&
          !au_cors_forbidden_request_header("X-HTTP-Method-Override", "PUT"));
    CHECK(au_cors_safelisted_request_header("Accept", "text/html") &&
          au_cors_safelisted_request_header("Content-Type", "text/plain; charset=UTF-8") &&
          au_cors_safelisted_request_header("content-type", "multipart/form-data; boundary=x") &&
          !au_cors_safelisted_request_header("Content-Type", "application/json") &&
          !au_cors_safelisted_request_header("Accept", "a(b)") && au_cors_safelisted_request_header("Range", "bytes=0-9") &&
          !au_cors_safelisted_request_header("Range", "bytes=-5") && !au_cors_no_cors_header("Range", "bytes=0-9") &&
          !au_cors_safelisted_request_header("X-A", "1"));
    char longv[200];
    memset(longv, 'a', 199);
    longv[199] = 0;
    CHECK(!au_cors_safelisted_request_header("Accept", longv));

    /* the CORS check */
    const char *star[] = {"Access-Control-Allow-Origin", "*"};
    const char *named[] = {"access-control-allow-origin", "https://a.test", "Access-Control-Allow-Credentials", "true"};
    const char *other[] = {"Access-Control-Allow-Origin", "https://b.test"};
    const char *twice[] = {"Access-Control-Allow-Origin", "https://a.test", "Access-Control-Allow-Origin", "https://a.test"};
    CHECK(au_cors_check("https://a.test", false, star, 1) && !au_cors_check("https://a.test", true, star, 1));
    CHECK(au_cors_check("https://a.test", true, named, 2) && !au_cors_check("https://a.test", true, named, 1));
    CHECK(!au_cors_check("https://a.test", false, other, 1) && !au_cors_check("https://a.test", false, NULL, 0));
    CHECK(!au_cors_check("https://a.test", false, twice, 2));
    CHECK(au_cors_check("null", false, star, 1));

    /* response header exposure */
    const char *exp[] = {"Access-Control-Expose-Headers", "X-One, x-two", "X-One", "1", "X-Three", "3"};
    const char *expstar[] = {"Access-Control-Expose-Headers", "*"};
    CHECK(au_cors_exposed_response_header("Content-Type", exp, 3, false) && au_cors_exposed_response_header("x-one", exp, 3, false) &&
          au_cors_exposed_response_header("X-Two", exp, 3, false) && !au_cors_exposed_response_header("X-Three", exp, 3, false) &&
          !au_cors_exposed_response_header("Set-Cookie", expstar, 1, false));
    CHECK(au_cors_exposed_response_header("X-Any", expstar, 1, false) && !au_cors_exposed_response_header("X-Any", expstar, 1, true));

    /* preflight */
    const char *req[] = {"X-B", "1", "Accept", "text/html", "x-a", "2", "Content-Type", "application/json"};
    AuBuf names;
    au_buf_init(&names);
    CHECK(au_cors_unsafe_header_names(req, 4, &names) && !strcmp(au_buf_cstr(&names), "content-type,x-a,x-b"));
    CHECK(!au_cors_unsafe_header_names(req + 2, 1, &names));
    au_buf_free(&names);
    const char *pre[] = {"Access-Control-Allow-Origin", "https://a.test", "Access-Control-Allow-Methods", "PUT, DELETE",
                         "Access-Control-Allow-Headers", "X-A, x-b, content-type", "Access-Control-Max-Age", "60"};
    const char *why = NULL;
    CHECK(au_cors_preflight_allows("https://a.test", false, "PUT", req, 4, 204, pre, 4, &why));
    CHECK(!au_cors_preflight_allows("https://a.test", false, "PATCH", req, 4, 200, pre, 4, &why) && why);
    CHECK(!au_cors_preflight_allows("https://a.test", false, "PUT", req, 4, 404, pre, 4, &why));
    CHECK(!au_cors_preflight_allows("https://a.test", false, "PUT", req, 4, 200, pre, 2, &why)); /* no Allow-Headers */
    CHECK(au_cors_preflight_allows("https://a.test", false, "POST", req + 2, 1, 200, pre, 1, &why)); /* safelisted */
    const char *prestar[] = {"Access-Control-Allow-Origin", "*", "Access-Control-Allow-Methods", "*",
                             "Access-Control-Allow-Headers", "*"};
    const char *auth[] = {"Authorization", "x"};
    CHECK(au_cors_preflight_allows("https://a.test", false, "PATCH", req, 4, 200, prestar, 3, &why));
    CHECK(!au_cors_preflight_allows("https://a.test", false, "GET", auth, 1, 200, prestar, 3, &why)); /* never by "*" */
    CHECK(au_cors_max_age(pre, 4) == 60 && au_cors_max_age(pre, 3) == 5);
    const char *age[] = {"Access-Control-Max-Age", "99999999"};
    CHECK(au_cors_max_age(age, 1) == AU_CORS_MAX_AGE);

    /* the preflight cache */
    AuCorsCache *c = au_cors_cache_new();
    CHECK(!au_cors_cache_allows(c, "https://a.test", "https://b.test/x", false, "PUT", req, 4, 0));
    au_cors_cache_store(c, "https://a.test", "https://b.test/x", false, pre, 4, 0);
    CHECK(au_cors_cache_allows(c, "https://a.test", "https://b.test/x", false, "PUT", req, 4, 59000));
    CHECK(!au_cors_cache_allows(c, "https://a.test", "https://b.test/x", false, "PATCH", req, 4, 1000));
    CHECK(!au_cors_cache_allows(c, "https://a.test", "https://b.test/y", false, "PUT", req, 4, 1000));
    CHECK(!au_cors_cache_allows(c, "https://a.test", "https://b.test/x", true, "PUT", req, 4, 1000));
    CHECK(!au_cors_cache_allows(c, "https://o.test", "https://b.test/x", false, "PUT", req, 4, 1000));
    CHECK(!au_cors_cache_allows(c, "https://a.test", "https://b.test/x", false, "PUT", req, 4, 60001)); /* expired */
    au_cors_cache_free(c);

    /* origins and Cross-Origin-Resource-Policy */
    CHECK(au_cors_same_origin("https://a.test/x", "https://a.test:443/y") && !au_cors_same_origin("https://a.test/", "http://a.test/") &&
          !au_cors_same_origin("data:text/plain,x", "data:text/plain,x") && au_cors_same_origin("file:///a", "file:///b"));
    CHECK(au_corp_allows(NULL, "https://a.test", "https://b.test/i") && au_corp_allows("cross-origin", "https://a.test", "https://b.test/i"));
    CHECK(!au_corp_allows("same-origin", "https://a.test", "https://b.test/i") &&
          au_corp_allows("same-origin", "https://a.test", "https://a.test/i"));
    CHECK(au_corp_allows("same-site", "https://www.example.com", "https://img.example.com/i") &&
          !au_corp_allows("same-site", "https://www.example.com", "https://example.org/i") &&
          !au_corp_allows("same-site", "http://www.example.com", "https://img.example.com/i"));

    /* digests */
    uint8_t d[AU_DIGEST_MAX];
    char h[2 * AU_DIGEST_MAX + 1];
    CHECK(au_digest(AU_DIGEST_SHA1, "abc", 3, d) && (hex(d, 20, h), !strcmp(h, "a9993e364706816aba3e25717850c26c9cd0d89d")));
    CHECK(au_digest(AU_DIGEST_SHA256, "abc", 3, d) &&
          (hex(d, 32, h), !strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
    CHECK(au_digest(AU_DIGEST_SHA384, "abc", 3, d) &&
          (hex(d, 48, h), !strcmp(h, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
                                     "8086072ba1e7cc2358baeca134c825a7")));
    CHECK(au_digest(AU_DIGEST_SHA512, "abc", 3, d) &&
          (hex(d, 64, h), !strcmp(h, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                                     "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f")));
    /* RFC 4231, test case 2 */
    CHECK(au_hmac(AU_DIGEST_SHA256, "Jefe", 4, "what do ya want for nothing?", 28, d) &&
          (hex(d, 32, h), !strcmp(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")));
    CHECK(au_hmac(AU_DIGEST_SHA512, "Jefe", 4, "what do ya want for nothing?", 28, d) &&
          (hex(d, 64, h), !strcmp(h, "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
                                     "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737")));
    CHECK(au_digest_by_name("sha-384") == AU_DIGEST_SHA384 && !au_digest_by_name("MD5"));
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
