/* Cookie store tests: RFC 6265 section 3.1 examples, attribute parsing,
 * domain and path matching, name prefixes, SameSite, third-party
 * blocking, partitioned cookies (CHIPS), limits, eviction, the persistent
 * file, document.cookie, and the store's use by the HTTP client. */
#include "fakenet.h"
#include "net/cookies.h"
#include "net/net.h"
#include "url/url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...)                                                                                          \
    do {                                                                                                          \
        checks++;                                                                                                 \
        if (!(cond)) {                                                                                            \
            failures++;                                                                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                           \
            printf(__VA_ARGS__);                                                                                  \
            printf("\n");                                                                                         \
        }                                                                                                         \
    } while (0)

#define NOW 1800000000LL

/* A request context built from strings. */
typedef struct Ctx {
    AuUrl url, site, init;
    bool has_site, has_init;
    AuCookieRequest rq;
} Ctx;

static void ctx_make(Ctx *c, const char *url, const char *site, const char *initiator, bool nav, bool safe)
{
    memset(c, 0, sizeof *c);
    bool ok = au_url_parse(&c->url, url, NULL);
    if (site)
        ok = ok && (c->has_site = au_url_parse(&c->site, site, NULL));
    if (initiator)
        ok = ok && (c->has_init = au_url_parse(&c->init, initiator, NULL));
    if (!ok) {
        printf("bad test URL %s\n", url);
        exit(2);
    }
    c->rq.url = &c->url;
    c->rq.site_for_cookies = c->has_site ? &c->site : NULL;
    c->rq.initiator = c->has_init ? &c->init : NULL;
    c->rq.top_level_nav = nav;
    c->rq.safe_method = safe;
    c->rq.now = NOW;
}

static void ctx_free(Ctx *c)
{
    au_url_free(&c->url);
    if (c->has_site)
        au_url_free(&c->site);
    if (c->has_init)
        au_url_free(&c->init);
}

/* Top-level navigation by the user to url. */
static bool set_top(AuCookieJar *jar, const char *url, const char *sc)
{
    Ctx c;
    ctx_make(&c, url, NULL, NULL, true, true);
    bool ok = au_cookie_jar_set(jar, &c.rq, sc, NULL);
    ctx_free(&c);
    return ok;
}

static bool set_at(AuCookieJar *jar, const char *url, int64_t now, const char *sc)
{
    Ctx c;
    ctx_make(&c, url, NULL, NULL, true, true);
    c.rq.now = now;
    bool ok = au_cookie_jar_set(jar, &c.rq, sc, NULL);
    ctx_free(&c);
    return ok;
}

/* The Cookie header for a user navigation to url ("" when none). */
static char *header_top(AuCookieJar *jar, const char *url)
{
    Ctx c;
    ctx_make(&c, url, NULL, NULL, true, true);
    AuBuf b;
    au_buf_init(&b);
    au_cookie_jar_header(jar, &c.rq, &b);
    ctx_free(&c);
    char *s = au_buf_take(&b, NULL);
    return s ? s : strdup("");
}

static char *header_at(AuCookieJar *jar, const char *url, int64_t now)
{
    Ctx c;
    ctx_make(&c, url, NULL, NULL, true, true);
    c.rq.now = now;
    AuBuf b;
    au_buf_init(&b);
    au_cookie_jar_header(jar, &c.rq, &b);
    ctx_free(&c);
    char *s = au_buf_take(&b, NULL);
    return s ? s : strdup("");
}

static char *header_ctx(AuCookieJar *jar, Ctx *c)
{
    AuBuf b;
    au_buf_init(&b);
    au_cookie_jar_header(jar, &c->rq, &b);
    char *s = au_buf_take(&b, NULL);
    return s ? s : strdup("");
}

#define EXPECT_HEADER(jar, url, want)                                                                             \
    do {                                                                                                          \
        char *got_ = header_top(jar, url);                                                                        \
        CHECK(strcmp(got_, want) == 0, "%s: got \"%s\", want \"%s\"", url, got_, want);                           \
        au_free(got_);                                                                                            \
    } while (0)

static void test_rfc_examples(void)
{
    AuCookieJar *jar = au_cookie_jar_new();

    /* 3.1: SID and lang. */
    CHECK(set_top(jar, "http://www.example.com/", "SID=31d4d96e407aad42"), "SID");
    EXPECT_HEADER(jar, "http://www.example.com/", "SID=31d4d96e407aad42");
    au_cookie_jar_clear(jar);

    CHECK(set_top(jar, "https://www.example.com/", "SID=31d4d96e407aad42; Path=/; Secure; HttpOnly"), "SID secure");
    CHECK(set_top(jar, "https://www.example.com/", "lang=en-US; Path=/; Domain=example.com"), "lang");
    EXPECT_HEADER(jar, "https://www.example.com/", "SID=31d4d96e407aad42; lang=en-US");
    /* Domain=example.com reaches the parent and other subdomains; the
     * host-only Secure SID does not, and is not sent over http. */
    EXPECT_HEADER(jar, "https://example.com/", "lang=en-US");
    EXPECT_HEADER(jar, "https://docs.example.com/", "lang=en-US");
    EXPECT_HEADER(jar, "http://www.example.com/", "lang=en-US");

    /* An expiry in the past without the same Domain names another
     * (host-only) cookie and leaves lang alone; with it, lang goes. */
    CHECK(set_top(jar, "https://www.example.com/", "lang=; Expires=Sun, 06 Nov 1994 08:49:37 GMT"), "delete other");
    EXPECT_HEADER(jar, "https://www.example.com/", "SID=31d4d96e407aad42; lang=en-US");
    CHECK(set_top(jar, "https://www.example.com/", "lang=; Path=/; Domain=example.com; Expires=Sun, 06 Nov 1994 "
                                                    "08:49:37 GMT"),
          "delete lang");
    EXPECT_HEADER(jar, "https://www.example.com/", "SID=31d4d96e407aad42");

    /* Expires in the future keeps the cookie past the session. */
    CHECK(set_top(jar, "https://www.example.com/", "lang=en-US; Expires=Wed, 09 Jun 2027 10:18:14 GMT"), "lang exp");
    au_cookie_jar_end_session(jar);
    EXPECT_HEADER(jar, "https://www.example.com/", "lang=en-US");
    au_cookie_jar_free(jar);
}

static void test_parsing(void)
{
    AuCookieJar *jar = au_cookie_jar_new();
    const char *why = NULL;
    Ctx c;
    ctx_make(&c, "https://a.example/x/y", NULL, NULL, true, true);

    CHECK(!au_cookie_jar_set(jar, &c.rq, "", &why), "empty");
    CHECK(!au_cookie_jar_set(jar, &c.rq, "=", &why), "lone =");
    CHECK(!au_cookie_jar_set(jar, &c.rq, "a=b\x01", &why), "control character");
    CHECK(!au_cookie_jar_set(jar, &c.rq, "a=b\x7f", &why), "DEL");
    CHECK(au_cookie_jar_set(jar, &c.rq, "  sp  =  v w  ; path=/", &why), "spaces trimmed");
    CHECK(au_cookie_jar_set(jar, &c.rq, "nameless", &why), "value only");
    CHECK(au_cookie_jar_set(jar, &c.rq, "q=\"quoted\"; Path=/", &why), "quotes kept");
    CHECK(au_cookie_jar_set(jar, &c.rq, "u=1; unknown=attr; Path=/; ; ;", &why), "unknown attribute");
    EXPECT_HEADER(jar, "https://a.example/", "sp=v w; q=\"quoted\"; u=1");
    /* Default path: the directory of the request path. */
    EXPECT_HEADER(jar, "https://a.example/x/z", "nameless; sp=v w; q=\"quoted\"; u=1");

    /* Size limits: name+value 4096, attribute 1024. */
    char *big = malloc(5000);
    memset(big, 'v', 4999);
    big[0] = 'n';
    big[1] = '=';
    big[4999] = 0;
    CHECK(!au_cookie_jar_set(jar, &c.rq, big, &why), "name+value over 4096");
    big[4098] = 0; /* "n" + 4096 bytes of value: 4097 */
    CHECK(!au_cookie_jar_set(jar, &c.rq, big, &why), "4097 bytes refused");
    big[4097] = 0; /* 4096 */
    CHECK(au_cookie_jar_set(jar, &c.rq, big, &why), "4096 bytes accepted: %s", why ? why : "");
    char attr[1200];
    snprintf(attr, sizeof attr, "p=1; Path=/%01100d", 0);
    CHECK(au_cookie_jar_set(jar, &c.rq, attr, &why), "over-long attribute is ignored, cookie kept");
    free(big);

    /* Max-Age wins over Expires; Max-Age<=0 deletes. */
    au_cookie_jar_clear(jar);
    CHECK(au_cookie_jar_set(jar, &c.rq, "m=1; Max-Age=60; Expires=Sun, 06 Nov 1994 08:49:37 GMT; Path=/", &why),
          "max-age wins");
    EXPECT_HEADER(jar, "https://a.example/", "m=1");
    char *h = header_at(jar, "https://a.example/", NOW + 61);
    CHECK(strcmp(h, "") == 0, "expired after max-age: %s", h);
    au_free(h);
    CHECK(au_cookie_jar_set(jar, &c.rq, "m=1; Max-Age=0; Path=/", &why), "max-age 0 deletes");
    EXPECT_HEADER(jar, "https://a.example/", "");
    CHECK(au_cookie_jar_set(jar, &c.rq, "m=1; Max-Age=abc; Path=/", &why), "bad max-age ignored");
    EXPECT_HEADER(jar, "https://a.example/", "m=1");

    /* Expiry capped at 400 days. */
    au_cookie_jar_clear(jar);
    CHECK(au_cookie_jar_set(jar, &c.rq, "far=1; Max-Age=999999999; Path=/", &why), "far");
    const AuCookie *ck = au_cookie_jar_count(jar) ? au_cookie_jar_at(jar, 0) : NULL;
    CHECK(ck && ck->expires == NOW + AU_COOKIE_MAX_AGE_SECONDS, "capped expiry %lld",
          ck ? (long long)ck->expires : 0LL);

    /* Cookie dates (RFC 6265 5.1.1). */
    au_cookie_jar_clear(jar);
    CHECK(au_cookie_jar_set(jar, &c.rq, "d=1; Expires=Thu, 01-Jan-2026 00:00:00 GMT; Path=/", &why), "past date");
    EXPECT_HEADER(jar, "https://a.example/", "");
    CHECK(au_cookie_jar_set(jar, &c.rq, "d=2; expires=Fri 31 Dec 2027 23:59:59; Path=/", &why), "loose date");
    EXPECT_HEADER(jar, "https://a.example/", "d=2");
    CHECK(au_cookie_jar_set(jar, &c.rq, "d=3; expires=31 Dec 70 23:59:59; Path=/", &why), "2-digit year 70");
    EXPECT_HEADER(jar, "https://a.example/", "");
    CHECK(au_cookie_jar_set(jar, &c.rq, "e=4; expires=garbage; Path=/", &why), "bad date ignored");
    EXPECT_HEADER(jar, "https://a.example/", "e=4");

    ctx_free(&c);
    au_cookie_jar_free(jar);
}

static void test_domain_path(void)
{
    AuCookieJar *jar = au_cookie_jar_new();
    CHECK(!set_top(jar, "https://www.example.com/", "a=1; Domain=other.com"), "foreign domain");
    CHECK(!set_top(jar, "https://www.example.com/", "a=1; Domain=com"), "public suffix");
    CHECK(!set_top(jar, "https://www.example.co.uk/", "a=1; Domain=co.uk"), "public suffix co.uk");
    CHECK(!set_top(jar, "https://example.com/", "a=1; Domain=www.example.com"), "subdomain of host");
    CHECK(!set_top(jar, "https://1.2.3.4/", "a=1; Domain=2.3.4"), "IP partial");
    CHECK(set_top(jar, "https://1.2.3.4/", "ip=1; Domain=1.2.3.4"), "IP exact");
    EXPECT_HEADER(jar, "https://1.2.3.4/", "ip=1");
    CHECK(set_top(jar, "https://www.example.com/", "d=1; Domain=.EXAMPLE.com"), "leading dot, case");
    EXPECT_HEADER(jar, "https://x.example.com/", "d=1");
    EXPECT_HEADER(jar, "https://notexample.com/", "");
    /* A host that is itself a public suffix may set host-only cookies. */
    CHECK(set_top(jar, "https://github.io/", "gh=1"), "host-only on suffix host");
    EXPECT_HEADER(jar, "https://github.io/", "gh=1");
    EXPECT_HEADER(jar, "https://x.github.io/", "");

    au_cookie_jar_clear(jar);
    CHECK(set_top(jar, "https://p.example/", "a=1; Path=/docs"), "path");
    EXPECT_HEADER(jar, "https://p.example/docs", "a=1");
    EXPECT_HEADER(jar, "https://p.example/docs/", "a=1");
    EXPECT_HEADER(jar, "https://p.example/docs/web/x", "a=1");
    EXPECT_HEADER(jar, "https://p.example/docsets", "");
    EXPECT_HEADER(jar, "https://p.example/", "");
    CHECK(set_top(jar, "https://p.example/", "b=1; Path=relative"), "relative path uses default");
    EXPECT_HEADER(jar, "https://p.example/", "b=1");

    /* Order: longer paths first, then earlier creation. */
    au_cookie_jar_clear(jar);
    set_at(jar, "https://o.example/", NOW, "first=1; Path=/");
    set_at(jar, "https://o.example/", NOW + 1, "deep=1; Path=/a/b");
    set_at(jar, "https://o.example/", NOW + 2, "mid=1; Path=/a");
    char *h = header_at(jar, "https://o.example/a/b/c", NOW + 3);
    CHECK(strcmp(h, "deep=1; mid=1; first=1") == 0, "order: %s", h);
    au_free(h);

    /* Same name, domain and path replaces; creation time is kept. */
    set_at(jar, "https://o.example/", NOW + 4, "first=2; Path=/");
    h = header_at(jar, "https://o.example/a/b/c", NOW + 5);
    CHECK(strcmp(h, "deep=1; mid=1; first=2") == 0, "replace keeps order: %s", h);
    au_free(h);
    au_cookie_jar_free(jar);
}

static void test_secure_and_prefixes(void)
{
    AuCookieJar *jar = au_cookie_jar_new();
    CHECK(!set_top(jar, "http://s.example/", "a=1; Secure"), "Secure from http");
    CHECK(set_top(jar, "https://s.example/", "a=1; Secure; Path=/"), "Secure from https");
    /* An insecure origin cannot overwrite (or shadow) a secure cookie. */
    CHECK(!set_top(jar, "http://s.example/", "a=2; Path=/"), "http cannot replace secure");
    CHECK(!set_top(jar, "http://s.example/", "a=2; Path=/sub"), "http cannot shadow secure");
    EXPECT_HEADER(jar, "https://s.example/", "a=1");
    EXPECT_HEADER(jar, "http://s.example/", "");
    /* localhost is trustworthy: Secure is allowed over http there. */
    CHECK(set_top(jar, "http://localhost/", "l=1; Secure"), "Secure on localhost");

    CHECK(!set_top(jar, "https://s.example/", "__Secure-x=1"), "__Secure- without Secure");
    CHECK(set_top(jar, "https://s.example/", "__Secure-x=1; Secure"), "__Secure-");
    CHECK(!set_top(jar, "http://s.example/", "__Secure-y=1; Secure"), "__Secure- from http");
    CHECK(!set_top(jar, "https://s.example/", "__Host-x=1; Secure"), "__Host- needs Path=/");
    CHECK(!set_top(jar, "https://s.example/", "__Host-x=1; Secure; Path=/; Domain=s.example"), "__Host- domain");
    CHECK(set_top(jar, "https://s.example/", "__Host-x=1; Secure; Path=/"), "__Host-");
    CHECK(!set_top(jar, "https://s.example/", "__host-z=1; Path=/"), "prefix is case-insensitive");
    /* Nameless cookies may not imitate a prefix. */
    CHECK(!set_top(jar, "https://s.example/", "__Host-n; Path=/"), "nameless __Host-");
    au_cookie_jar_free(jar);
}

static void test_http_only(void)
{
    AuCookieJar *jar = au_cookie_jar_new();
    set_top(jar, "https://h.example/", "h=1; HttpOnly; Path=/");
    set_top(jar, "https://h.example/", "j=1; Path=/");
    Ctx c;
    ctx_make(&c, "https://h.example/", NULL, NULL, true, true);
    AuBuf b;
    au_buf_init(&b);
    au_cookie_jar_document_get(jar, &c.rq, &b);
    CHECK(strcmp(au_buf_cstr(&b), "j=1") == 0, "document.cookie hides HttpOnly: %s", au_buf_cstr(&b));
    au_cookie_jar_document_set(jar, &c.rq, "h=2");
    au_cookie_jar_document_set(jar, &c.rq, "k=3; HttpOnly");
    au_cookie_jar_document_set(jar, &c.rq, "s=4");
    au_buf_clear(&b);
    au_cookie_jar_document_get(jar, &c.rq, &b);
    CHECK(strcmp(au_buf_cstr(&b), "j=1; s=4") == 0, "script cannot touch HttpOnly: %s", au_buf_cstr(&b));
    EXPECT_HEADER(jar, "https://h.example/", "h=1; j=1; s=4");
    au_buf_free(&b);
    ctx_free(&c);
    au_cookie_jar_free(jar);
}

static void test_same_site(void)
{
    AuCookieJar *jar = au_cookie_jar_new();
    au_cookie_jar_set_policy(jar, AU_COOKIE_POLICY_ALL, false); /* SameSite alone, no third-party blocking */
    set_top(jar, "https://site.example/", "def=1; Path=/");
    set_top(jar, "https://site.example/", "lax=1; SameSite=Lax; Path=/");
    set_top(jar, "https://site.example/", "strict=1; SameSite=Strict; Path=/");
    CHECK(set_top(jar, "https://site.example/", "none=1; SameSite=None; Secure; Path=/"), "None+Secure");
    CHECK(!set_top(jar, "https://site.example/", "bad=1; SameSite=None; Path=/"), "None without Secure");
    CHECK(set_top(jar, "https://site.example/", "odd=1; SameSite=Bogus; Path=/"), "unknown SameSite: default");

    Ctx c;
    char *h;
    /* Same-site subresource. */
    ctx_make(&c, "https://img.site.example/", "https://www.site.example/", NULL, false, true);
    set_top(jar, "https://img.site.example/", "x=1; Domain=site.example; Path=/");
    h = header_ctx(jar, &c);
    CHECK(strstr(h, "def=1") == NULL, "host-only def not on img: %s", h);
    au_free(h);
    ctx_free(&c);

    ctx_make(&c, "https://site.example/", "https://site.example/", NULL, false, true);
    h = header_ctx(jar, &c);
    CHECK(strcmp(h, "def=1; lax=1; strict=1; none=1; odd=1; x=1") == 0, "same-site: %s", h);
    au_free(h);
    ctx_free(&c);

    /* Cross-site subresource: only None. */
    ctx_make(&c, "https://site.example/", "https://other.example/", NULL, false, true);
    h = header_ctx(jar, &c);
    CHECK(strcmp(h, "none=1") == 0, "cross-site subresource: %s", h);
    au_free(h);
    ctx_free(&c);

    /* Cross-site top-level GET navigation: Lax and default, not Strict. */
    ctx_make(&c, "https://site.example/", NULL, "https://other.example/", true, true);
    h = header_ctx(jar, &c);
    CHECK(strcmp(h, "def=1; lax=1; none=1; odd=1; x=1") == 0, "cross-site nav: %s", h);
    au_free(h);
    ctx_free(&c);

    /* Cross-site top-level POST: only None. */
    ctx_make(&c, "https://site.example/", NULL, "https://other.example/", true, false);
    h = header_ctx(jar, &c);
    CHECK(strcmp(h, "none=1") == 0, "cross-site POST nav: %s", h);
    au_free(h);
    ctx_free(&c);

    /* Same-site navigation started by the site itself: everything. */
    ctx_make(&c, "https://site.example/", NULL, "https://www.site.example/", true, false);
    h = header_ctx(jar, &c);
    CHECK(strcmp(h, "def=1; lax=1; strict=1; none=1; odd=1; x=1") == 0, "same-site POST nav: %s", h);
    au_free(h);
    ctx_free(&c);

    /* Scheme matters for site: http is cross-site with https. */
    ctx_make(&c, "https://site.example/", "http://site.example/", NULL, false, true);
    h = header_ctx(jar, &c);
    CHECK(strcmp(h, "none=1") == 0, "schemeful same-site: %s", h);
    au_free(h);
    ctx_free(&c);

    /* Setting from a cross-site subresource: Lax/Strict refused, None ok. */
    ctx_make(&c, "https://site.example/", "https://other.example/", NULL, false, true);
    CHECK(!au_cookie_jar_set(jar, &c.rq, "cs=1; SameSite=Strict; Path=/", NULL), "cross-site set Strict");
    CHECK(!au_cookie_jar_set(jar, &c.rq, "cs=1; Path=/", NULL), "cross-site set default (Lax)");
    CHECK(au_cookie_jar_set(jar, &c.rq, "cs=1; SameSite=None; Secure; Path=/", NULL), "cross-site set None");
    ctx_free(&c);
    au_cookie_jar_free(jar);
}

static void test_third_party(void)
{
    AuCookieJar *jar = au_cookie_jar_new(); /* default: third-party blocked */
    Ctx c;
    char *h;

    CHECK(set_top(jar, "https://tracker.example/", "t=1; SameSite=None; Secure; Path=/"), "first-party set");

    ctx_make(&c, "https://tracker.example/pixel", "https://news.example/", NULL, false, true);
    CHECK(au_cookie_is_third_party(&c.rq), "is third party");
    h = header_ctx(jar, &c);
    CHECK(strcmp(h, "") == 0, "third-party not sent: %s", h);
    au_free(h);
    CHECK(!au_cookie_jar_set(jar, &c.rq, "t2=1; SameSite=None; Secure; Path=/", NULL), "third-party not stored");
    ctx_free(&c);

    /* Subdomains of the same registrable domain are first party. */
    ctx_make(&c, "https://cdn.news.example/", "https://www.news.example/", NULL, false, true);
    CHECK(!au_cookie_is_third_party(&c.rq), "same registrable domain");
    ctx_free(&c);
    /* co.uk: different registrable domains under a multi-label suffix. */
    ctx_make(&c, "https://a.co.uk/", "https://b.co.uk/", NULL, false, true);
    CHECK(au_cookie_is_third_party(&c.rq), "a.co.uk vs b.co.uk");
    ctx_free(&c);
    /* Top-level navigation is never third party. */
    ctx_make(&c, "https://tracker.example/", NULL, "https://news.example/", true, true);
    CHECK(!au_cookie_is_third_party(&c.rq), "navigation");
    h = header_ctx(jar, &c);
    CHECK(strcmp(h, "t=1") == 0, "first-party cookie on navigation: %s", h);
    au_free(h);
    ctx_free(&c);

    /* CHIPS: a third-party Partitioned; Secure cookie is accepted, keyed
     * by the top-level site, and sent only under that site. */
    ctx_make(&c, "https://embed.example/w", "https://news.example/a", NULL, false, true);
    CHECK(au_cookie_jar_set(jar, &c.rq, "p=1; Partitioned; Secure; SameSite=None; Path=/", NULL), "CHIPS stored");
    CHECK(!au_cookie_jar_set(jar, &c.rq, "q=1; Partitioned; SameSite=None; Path=/", NULL),
          "Partitioned without Secure");
    h = header_ctx(jar, &c);
    CHECK(strcmp(h, "p=1") == 0, "CHIPS sent under its top site: %s", h);
    au_free(h);
    ctx_free(&c);

    ctx_make(&c, "https://embed.example/w", "https://www.news.example/", NULL, false, true);
    h = header_ctx(jar, &c);
    CHECK(strcmp(h, "p=1") == 0, "CHIPS: same top site, other host: %s", h);
    au_free(h);
    ctx_free(&c);

    ctx_make(&c, "https://embed.example/w", "https://shop.example/", NULL, false, true);
    h = header_ctx(jar, &c);
    CHECK(strcmp(h, "") == 0, "CHIPS not sent under another top site: %s", h);
    au_free(h);
    ctx_free(&c);

    /* Nor on a visit to embed.example itself. */
    EXPECT_HEADER(jar, "https://embed.example/", "");

    /* The partition is recorded. */
    bool found = false;
    for (size_t i = 0; i < au_cookie_jar_count(jar); i++) {
        const AuCookie *ck = au_cookie_jar_at(jar, i);
        if (strcmp(ck->name, "p") == 0) {
            found = true;
            CHECK(ck->partitioned && strcmp(ck->partition, "https://news.example") == 0, "partition key %s",
                  ck->partition);
        }
        if (strcmp(ck->name, "t") == 0)
            CHECK(!ck->partitioned && strcmp(ck->partition, "") == 0, "unpartitioned key");
    }
    CHECK(found, "partitioned cookie listed");

    /* A partitioned cookie set first-party is keyed by its own site. */
    CHECK(set_top(jar, "https://embed.example/", "own=1; Partitioned; Secure; Path=/"), "first-party partitioned");
    EXPECT_HEADER(jar, "https://embed.example/", "own=1");
    ctx_make(&c, "https://embed.example/w", "https://news.example/a", NULL, false, true);
    h = header_ctx(jar, &c);
    CHECK(strcmp(h, "p=1") == 0, "own-site partition not sent under news: %s", h);
    au_free(h);
    ctx_free(&c);

    /* With blocking off, third-party SameSite=None cookies flow. */
    au_cookie_jar_set_policy(jar, AU_COOKIE_POLICY_ALL, false);
    ctx_make(&c, "https://tracker.example/pixel", "https://news.example/", NULL, false, true);
    h = header_ctx(jar, &c);
    CHECK(strcmp(h, "t=1") == 0, "blocking off: %s", h);
    au_free(h);
    ctx_free(&c);

    /* Policy NONE: nothing stored or sent. */
    au_cookie_jar_set_policy(jar, AU_COOKIE_POLICY_NONE, true);
    CHECK(!set_top(jar, "https://x.example/", "z=1"), "policy none");
    EXPECT_HEADER(jar, "https://tracker.example/", "");
    au_cookie_jar_free(jar);
}

static void test_limits(void)
{
    AuCookieJar *jar = au_cookie_jar_new();
    char sc[64];
    for (int i = 0; i < AU_COOKIES_PER_DOMAIN + 20; i++) {
        snprintf(sc, sizeof sc, "c%d=%d; Path=/", i, i);
        set_at(jar, "https://many.example/", NOW + i, sc);
    }
    size_t n = 0;
    bool have_first = false, have_last = false;
    for (size_t i = 0; i < au_cookie_jar_count(jar); i++) {
        const AuCookie *ck = au_cookie_jar_at(jar, i);
        if (strcmp(ck->domain, "many.example") == 0)
            n++;
        if (strcmp(ck->name, "c0") == 0)
            have_first = true;
        if (strcmp(ck->name, "c169") == 0)
            have_last = true;
    }
    CHECK(n == AU_COOKIES_PER_DOMAIN, "per-domain cap: %zu", n);
    CHECK(!have_first && have_last, "least recently used evicted");

    /* Total cap. */
    au_cookie_jar_clear(jar);
    char url[64];
    for (int d = 0; d < AU_COOKIES_TOTAL / 100 + 5; d++) {
        snprintf(url, sizeof url, "https://d%d.example/", d);
        for (int i = 0; i < 100; i++) {
            snprintf(sc, sizeof sc, "c%d=1; Path=/", i);
            set_at(jar, url, NOW + d * 100 + i, sc);
        }
    }
    CHECK(au_cookie_jar_count(jar) <= AU_COOKIES_TOTAL, "total cap: %zu", au_cookie_jar_count(jar));

    /* remove_domain removes the host and its subdomains. */
    au_cookie_jar_clear(jar);
    set_top(jar, "https://www.r.example/", "a=1; Domain=r.example");
    set_top(jar, "https://www.r.example/", "b=1");
    set_top(jar, "https://keep.example/", "c=1");
    au_cookie_jar_remove_domain(jar, "r.example");
    CHECK(au_cookie_jar_count(jar) == 1, "remove_domain: %zu left", au_cookie_jar_count(jar));
    au_cookie_jar_free(jar);
}

static void test_persistence(void)
{
    AuCookieJar *jar = au_cookie_jar_new();
    CHECK(!au_cookie_jar_dirty(jar), "new jar clean");
    set_top(jar, "https://p.example/", "keep=v\tal%ue; Max-Age=3600; Path=/; HttpOnly; SameSite=Strict");
    set_top(jar, "https://p.example/", "sess=1; Path=/");
    Ctx c;
    ctx_make(&c, "https://embed.example/", "https://news.example/", NULL, false, true);
    au_cookie_jar_set(jar, &c.rq, "part=1; Partitioned; Secure; SameSite=None; Path=/; Max-Age=86400", NULL);
    ctx_free(&c);
    CHECK(au_cookie_jar_dirty(jar), "dirty after set");

    AuBuf b;
    au_buf_init(&b);
    au_cookie_jar_serialize(jar, &b, NOW);
    char *text = au_buf_cstr(&b);
    CHECK(strncmp(text, "# Aura cookies 1\n", 17) == 0, "header line");
    CHECK(strstr(text, "sess") == NULL, "session cookie not written");

    AuCookieJar *j2 = au_cookie_jar_new();
    CHECK(au_cookie_jar_parse(j2, text, strlen(text), NOW), "parse back");
    CHECK(au_cookie_jar_count(j2) == 2, "two persisted: %zu", au_cookie_jar_count(j2));
    EXPECT_HEADER(j2, "https://p.example/", "keep=v\tal%ue");
    ctx_make(&c, "https://embed.example/", "https://news.example/", NULL, false, true);
    char *h = header_ctx(j2, &c);
    CHECK(strcmp(h, "part=1") == 0, "partitioned cookie survives: %s", h);
    au_free(h);
    ctx_free(&c);
    /* Expired by load time: dropped. */
    AuCookieJar *j3 = au_cookie_jar_new();
    au_cookie_jar_parse(j3, text, strlen(text), NOW + 7200);
    CHECK(au_cookie_jar_count(j3) == 1, "expired dropped on load: %zu", au_cookie_jar_count(j3));

    /* Damaged input: good lines kept, false returned, no crash. */
    AuCookieJar *j4 = au_cookie_jar_new();
    const char *bad = "# Aura cookies 1\n"
                      "p.example\t1\t/\t1\t0\tlax\t0\t\t1900000000\t1\t1\tok\t1\n"
                      "garbage line\n"
                      "p.example\t1\t/\t1\t0\tlax\t0\t\tnotanumber\t1\t1\tbad\t1\n"
                      "p.example\t1\tnoslash\t1\t0\tlax\t0\t\t1900000000\t1\t1\tbad2\t1\n";
    CHECK(!au_cookie_jar_parse(j4, bad, strlen(bad), NOW), "damaged file reported");
    CHECK(au_cookie_jar_count(j4) == 1, "good line kept: %zu", au_cookie_jar_count(j4));
    CHECK(!au_cookie_jar_parse(j4, "not a cookie file", 17, NOW), "wrong header");

    au_buf_free(&b);
    au_cookie_jar_free(jar);
    au_cookie_jar_free(j2);
    au_cookie_jar_free(j3);
    au_cookie_jar_free(j4);
}

/* The HTTP client sends and stores cookies through the jar. */
typedef struct R {
    bool done;
    AuNetError err;
} R;
static void r_done(void *ctx, AuHttpTransfer *t, AuNetError e)
{
    (void)t;
    R *r = ctx;
    r->done = true;
    r->err = e;
}

static void test_http_integration(void)
{
    FakeNet f;
    fake_init(&f);
    FakeServer *s = fake_server(&f, "c.example", 80);
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nSet-Cookie: id=42; Path=/\r\n"
                    "Set-Cookie: tp=1; SameSite=None; Secure\r\n\r\n");
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    AuNet *net = au_net_new(&f.ops);
    AuCookieJar *jar = au_cookie_jar_new();
    au_net_set_cookie_jar(net, jar);

    for (int i = 0; i < 2; i++) {
        R r = {0};
        AuHttpRequest rq;
        au_http_request_init(&rq, "http://c.example/page");
        AuHttpCallbacks cb = {0};
        cb.ctx = &r;
        cb.done = r_done;
        au_http_start(net, &rq, &cb);
        au_http_request_free(&rq);
        fake_run(&f, net, 200, 10);
        CHECK(r.done && r.err == AU_NET_OK, "request %d: err %d", i, (int)r.err);
    }
    const char *second = s->nreq > 1 ? s->requests[1] : NULL;
    CHECK(second && strstr(second, "\r\nCookie: id=42\r\n"), "cookie sent on second request: %s",
          second ? second : "(none)");
    const char *first = s->nreq > 0 ? s->requests[0] : NULL;
    CHECK(first && !strstr(first, "Cookie:"), "no cookie on first request");
    au_net_free(net);
    au_cookie_jar_free(jar);
    fake_free(&f);
}

int main(void)
{
    test_rfc_examples();
    test_parsing();
    test_domain_path();
    test_secure_and_prefixes();
    test_http_only();
    test_same_site();
    test_third_party();
    test_limits();
    test_persistence();
    test_http_integration();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
