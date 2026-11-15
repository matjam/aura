/* URL parser tests: our own cases (from the URL Standard's examples and
 * the tricky cases), Punycode, origins, and, when the web-platform-tests
 * data is present, urltestdata.json.
 *
 * usage: test_url                       our cases
 *        test_url --wpt FILE EXPECTED   the WPT data; exits 77 when FILE
 *                                       is absent. EXPECTED lists inputs
 *                                       known to fail (one per line, as
 *                                       JSON-escaped text); any other
 *                                       failure fails the test. */
#include "url/url.h"
#include "../html/json.h"

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

typedef struct Case {
    const char *input, *base, *href; /* href NULL: failure expected */
} Case;

static const Case cases[] = {
    /* The standard's examples */
    {"https:example.org", NULL, "https://example.org/"},
    {"https://////example.com///", NULL, "https://example.com///"},
    {"https://example.com/././foo", NULL, "https://example.com/foo"},
    {"hello:world", "https://example.com/", "hello:world"},
    {"https:example.org", "https://example.com/", "https://example.com/example.org"},
    {"\\example\\..\\demo/.\\", "https://example.com/", "https://example.com/demo/"},
    {"example", "https://example.com/demo", "https://example.com/example"},
    {"file:///C|/demo", NULL, "file:///C:/demo"},
    {"..", "file:///C:/demo", "file:///C:/"},
    {"file://loc%61lhost/", NULL, "file:///"},
    {"https://user:password@example.org/", NULL, "https://user:password@example.org/"},
    {"https://example.org/foo bar", NULL, "https://example.org/foo%20bar"},
    {"https://EXAMPLE.com/../x", NULL, "https://example.com/x"},
    {"https://ex ample.org/", NULL, NULL},
    {"example", NULL, NULL},
    {"https://example.com:demo", NULL, NULL},
    {"http://[www.example.com]/", NULL, NULL},
    {"https://example.org//", NULL, "https://example.org//"},
    {"https://example.com/[]?[]#[]", NULL, "https://example.com/[]?[]#[]"},
    {"https://example/%?%#%", NULL, "https://example/%?%#%"},
    {"https://example/%25?%25#%25", NULL, "https://example/%25?%25#%25"},
    /* Relative forms */
    {"//other.example/x", "https://a.example/b/c", "https://other.example/x"},
    {"/abs", "https://a.example/b/c?q#f", "https://a.example/abs"},
    {"?nq", "https://a.example/b/c?q#f", "https://a.example/b/c?nq"},
    {"#nf", "https://a.example/b/c?q#f", "https://a.example/b/c?q#nf"},
    {"", "https://a.example/b/c?q#f", "https://a.example/b/c?q"},
    {"d/../../e", "https://a.example/b/c", "https://a.example/e"},
    {"%2e%2E/x", "https://a.example/b/c", "https://a.example/x"},
    {".%2e/x", "https://a.example/b/c/d", "https://a.example/b/x"},
    {"g;x=1/../y", "http://a/b/c/d;p?q", "http://a/b/c/y"},
    {"../../../../g", "http://a/b/c/d;p?q", "http://a/g"},
    /* Hosts */
    {"http://0x7f.1/", NULL, "http://127.0.0.1/"},
    {"http://192.168.0.257/", NULL, NULL},
    {"http://1.2.3.4.5/", NULL, NULL},
    {"http://4294967295/", NULL, "http://255.255.255.255/"},
    {"http://4294967296/", NULL, NULL},
    {"http://0300.0250.0.1/", NULL, "http://192.168.0.1/"},
    {"http://09.1/", NULL, NULL},
    {"http://foo.09/", NULL, NULL},
    {"http://1.2.3.4./", NULL, "http://1.2.3.4/"},
    {"http://[::1]/", NULL, "http://[::1]/"},
    {"http://[0:0:0:0:0:0:0:1]:8080/", NULL, "http://[::1]:8080/"},
    {"http://[1:0::0:0:7]/", NULL, "http://[1::7]/"},
    {"http://[::ffff:192.168.0.1]/", NULL, "http://[::ffff:c0a8:1]/"},
    {"http://[1:2:3:4:5:6:7:8:9]/", NULL, NULL},
    {"http://[1::2::3]/", NULL, NULL},
    {"http://[::1.2.3.]/", NULL, NULL},
    {"http://[0:1:0:1:0:0:0:0]/", NULL, "http://[0:1:0:1::]/"},
    {"http://a%41b.com/", NULL, "http://aab.com/"},
    {"http://a%2fb.com/", NULL, NULL},
    {"http://ex%00ample/", NULL, NULL},
    {"http://bücher.example/", NULL, "http://xn--bcher-kva.example/"},
    {"http://xn--bcher-kva.example/", NULL, "http://xn--bcher-kva.example/"},
    {"http://xn--a.example/", NULL, NULL},
    {"http://EXAMPLE。com/", NULL, "http://example.com/"},
    {"http://ＥＸＡＭＰＬＥ.com/", NULL, "http://example.com/"},
    {"http://a<b/", NULL, NULL},
    {"non-special://a<b/", NULL, NULL},
    {"non-special://H%41st/p", NULL, "non-special://H%41st/p"},
    {"http:///", NULL, NULL},
    {"http://user@/", NULL, NULL},
    /* Ports */
    {"http://h:80/", NULL, "http://h/"},
    {"https://h:443/", NULL, "https://h/"},
    {"http://h:443/", NULL, "http://h:443/"},
    {"http://h:00080/", NULL, "http://h/"},
    {"http://h:65535/", NULL, "http://h:65535/"},
    {"http://h:65536/", NULL, NULL},
    {"http://h:99999999999999999999/", NULL, NULL},
    {"ws://h:80/", NULL, "ws://h/"},
    {"ftp://h:21/", NULL, "ftp://h/"},
    {"foo://h:80/", NULL, "foo://h:80/"},
    /* Userinfo */
    {"http://a:b@c:d@h/", NULL, "http://a:b%40c%3Ad@h/"},
    {"http://@h/", NULL, "http://h/"},
    {"http://:@h/", NULL, "http://h/"},
    {"http://us er@h/", NULL, "http://us%20er@h/"},
    /* Opaque paths and other schemes */
    {"data:text/plain,hello world", NULL, "data:text/plain,hello world"},
    {"data:text/plain,a ?b", NULL, "data:text/plain,a%20?b"},
    {"about:blank", NULL, "about:blank"},
    {"mailto:a@b.c", NULL, "mailto:a@b.c"},
    {"javascript:alert(1)", NULL, "javascript:alert(1)"},
    {"#x", "about:blank", "about:blank#x"},
    {"x", "about:blank", NULL},
    {"web+demo:/.//not-a-host/", NULL, "web+demo:/.//not-a-host/"},
    {"sc://ñ/", NULL, "sc://%C3%B1/"},
    /* Queries and fragments */
    {"http://h/?a'b", NULL, "http://h/?a%27b"},
    {"foo://h/?a'b", NULL, "foo://h/?a'b"},
    {"http://h/#a b<c>", NULL, "http://h/#a%20b%3Cc%3E"},
    {"http://h/?é", NULL, "http://h/?%C3%A9"},
    {"http://h/p\x01q", NULL, "http://h/p%01q"},
    {" \t http://h/a\nb \x01", NULL, "http://h/ab"},
    /* file: */
    {"file:///etc/passwd", NULL, "file:///etc/passwd"},
    {"file://host/share", NULL, "file://host/share"},
    {"file:c:\\foo\\bar.html", "file:///tmp/mock/path", "file:///c:/foo/bar.html"},
    {"/c:/foo/bar", "file:///c:/baz/qux", "file:///c:/foo/bar"},
    {"..", "file:///", "file:///"},
    {"file://localhost/x", NULL, "file:///x"},
    /* Invalid UTF-8 becomes U+FFFD */
    {"http://h/\xff", NULL, "http://h/%EF%BF%BD"},
};

static void own_cases(void)
{
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const Case *c = &cases[i];
        AuUrl base, u;
        bool have_base = false;
        if (c->base) {
            have_base = au_url_parse(&base, c->base, NULL);
            CHECK(have_base, "base %s", c->base);
        }
        bool ok = au_url_parse(&u, c->input, have_base ? &base : NULL);
        if (!c->href) {
            CHECK(!ok, "\"%s\" should fail, gave %s", c->input, ok ? u.href : "");
        } else {
            CHECK(ok && !strcmp(u.href, c->href), "\"%s\" -> %s, expected %s", c->input, ok ? u.href : "(failure)",
                  c->href);
            if (ok) {
                /* Reparsing the serialization gives the same URL. */
                AuUrl v;
                bool ok2 = au_url_parse(&v, u.href, NULL);
                CHECK(ok2 && !strcmp(v.href, u.href), "reparse of %s", u.href);
                if (ok2)
                    au_url_free(&v);
            }
        }
        if (ok)
            au_url_free(&u);
        if (have_base)
            au_url_free(&base);
    }
}

static void components(void)
{
    AuUrl u;
    CHECK(au_url_parse(&u, "https://user:pw@Example.COM:8443/a/b?q=1#frag", NULL), "parse");
    CHECK(!strcmp(u.scheme, "https") && !strcmp(u.username, "user") && !strcmp(u.password, "pw"), "creds");
    CHECK(!strcmp(u.host, "example.com") && u.port == 8443 && !strcmp(u.path, "/a/b"), "host/port/path");
    CHECK(u.query && !strcmp(u.query, "q=1") && u.fragment && !strcmp(u.fragment, "frag"), "query/fragment");
    CHECK(u.special && !u.opaque_path && u.host_type == AU_URL_HOST_DOMAIN, "flags");
    CHECK(au_url_effective_port(&u) == 8443, "effective port");
    AuOrigin o, o2;
    au_url_origin(&u, &o);
    AuBuf b;
    au_buf_init(&b);
    au_origin_serialize(&o, &b);
    CHECK(!strcmp(au_buf_cstr(&b), "https://example.com:8443"), "origin %s", b.data);
    AuUrl v;
    au_url_parse(&v, "https://example.com:8443/other", NULL);
    au_url_origin(&v, &o2);
    CHECK(au_origin_same(&o, &o2), "same origin");
    au_origin_free(&o2);
    au_url_free(&v);
    au_url_parse(&v, "data:,x", NULL);
    au_url_origin(&v, &o2);
    CHECK(o2.opaque && !au_origin_same(&o2, &o2), "data: origin is opaque");
    au_buf_clear(&b);
    au_origin_serialize(&o2, &b);
    CHECK(!strcmp(au_buf_cstr(&b), "null"), "opaque serializes as null");
    au_origin_free(&o2);
    au_url_free(&v);
    au_url_parse(&v, "blob:https://example.com:8443/uuid", NULL);
    au_url_origin(&v, &o2);
    CHECK(au_origin_same(&o, &o2), "blob origin");
    au_origin_free(&o2);
    au_url_free(&v);
    au_origin_free(&o);
    au_url_free(&u);

    au_url_parse(&u, "http://[::ffff:1.2.3.4]/", NULL);
    CHECK(u.host_type == AU_URL_HOST_IPV6 && u.ipv6[5] == 0xffff && u.ipv6[6] == 0x0102, "ipv6 pieces");
    au_url_free(&u);
    au_url_parse(&u, "http://10.0.0.1/", NULL);
    CHECK(u.host_type == AU_URL_HOST_IPV4 && u.ipv4 == 0x0A000001u, "ipv4 value");
    au_url_free(&u);

    char *r = au_url_resolve_str("http://a/b/c", "../d");
    CHECK(r && !strcmp(r, "http://a/d"), "resolve_str %s", r ? r : "(null)");
    au_free(r);
    r = au_url_resolve_str(NULL, "nope");
    CHECK(!r, "resolve_str failure");

    au_buf_clear(&b);
    au_pct_encode(&b, "a b&c=d/é", strlen("a b&c=d/é"), AU_PCT_COMPONENT);
    CHECK(!strcmp(au_buf_cstr(&b), "a%20b%26c%3Dd%2F%C3%A9"), "component encode %s", b.data);
    au_buf_clear(&b);
    au_pct_decode(&b, "%41%zz%4", 8);
    CHECK(b.len == 6 && !memcmp(b.data, "A%zz%4", 6), "pct decode");
    au_buf_free(&b);
}

static void punycode(void)
{
    /* RFC 3492 7.1 sample (A) Arabic (Egyptian) and others. */
    static const uint32_t arabic[] = {0x0644, 0x064A, 0x0647, 0x0645, 0x0627, 0x0628, 0x062A, 0x0643, 0x0644,
                                      0x0645, 0x0648, 0x0634, 0x0639, 0x0631, 0x0628, 0x064A, 0x061F};
    AuBuf b;
    au_buf_init(&b);
    CHECK(au_punycode_encode(arabic, 17, &b) && !strcmp(au_buf_cstr(&b), "egbpdaj6bu4bxfgehfvwxn"), "arabic %s",
          b.data);
    uint32_t out[64];
    int n = au_punycode_decode("egbpdaj6bu4bxfgehfvwxn", 22, out, 64);
    CHECK(n == 17 && !memcmp(out, arabic, sizeof arabic), "arabic decode");
    static const uint32_t mixed[] = {'3', 0x5E74, 'B', 0x7D44, 0x91D1, 0x516B, 0x5148, 0x751F};
    au_buf_clear(&b);
    CHECK(au_punycode_encode(mixed, 8, &b) && !strcmp(au_buf_cstr(&b), "3B-ww4c5e180e575a65lsy2b"), "mixed %s",
          b.data);
    CHECK(au_punycode_decode("3B-ww4c5e180e575a65lsy2b", 24, out, 64) == 8, "mixed decode");
    /* Overflow and bad digits */
    CHECK(au_punycode_decode("99999999999999999", 17, out, 64) < 0, "overflow");
    CHECK(au_punycode_decode("a-!", 3, out, 64) < 0, "bad digit");
    CHECK(au_punycode_decode("egbpdaj6bu4bxfgehfvwxn", 22, out, 4) < 0, "capacity");
    au_buf_free(&b);
}

/* ---- WPT ---- */

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = malloc((size_t)n + 1);
    if (fread(p, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(p);
        return NULL;
    }
    fclose(f);
    p[n] = 0;
    *len = (size_t)n;
    return p;
}

static const char *jstr(const JVal *o, const char *k)
{
    JVal *v = json_get(o, k);
    return v && v->type == J_STR ? v->str : NULL;
}

/* Escapes s so it fits on one line of the expected-failures file. */
static void escape(const char *s, AuBuf *out)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == '\\' || c == 0x7f)
            au_buf_printf(out, "\\x%02x", c);
        else
            au_buf_putc(out, (char)c);
    }
}

static int wpt(const char *path, const char *expected_path)
{
    size_t len;
    char *data = read_all(path, &len);
    if (!data) {
        printf("skipped: %s not present (see tests/url/CMakeLists.txt)\n", path);
        return 77;
    }
    size_t elen = 0;
    char *expected = read_all(expected_path, &elen);
    JVal *root = json_parse(data, len);
    if (!root || root->type != J_ARR) {
        printf("cannot parse %s\n", path);
        return 1;
    }
    int pass = 0, fail = 0, unexpected = 0, expected_fail = 0;
    for (size_t i = 0; i < root->n; i++) {
        JVal *t = root->items[i];
        if (t->type != J_OBJ)
            continue;
        JVal *in = json_get(t, "input");
        JVal *basev = json_get(t, "base");
        if (!in || in->type != J_STR)
            continue;
        if (memchr(in->str, 0, in->len))
            continue; /* NUL in input: our API takes C strings */
        bool want_fail = json_get(t, "failure") && json_get(t, "failure")->type == J_TRUE;
        AuUrl base, u;
        bool have_base = false;
        char reason[512] = "";
        bool ok;
        if (basev && basev->type == J_STR) {
            have_base = au_url_parse(&base, basev->str, NULL);
            if (!have_base) {
                snprintf(reason, sizeof reason, "base did not parse");
            }
        }
        ok = (!basev || basev->type != J_STR || have_base) && au_url_parse_n(&u, in->str, in->len, have_base ? &base : NULL);
        bool good;
        if (want_fail) {
            good = !ok;
            if (!good)
                snprintf(reason, sizeof reason, "should fail, gave %s", u.href);
        } else if (!ok) {
            good = false;
            if (!reason[0])
                snprintf(reason, sizeof reason, "failed to parse");
        } else {
            good = true;
            const char *href = jstr(t, "href"), *proto = jstr(t, "protocol"), *user = jstr(t, "username"),
                       *pass_ = jstr(t, "password"), *hostname = jstr(t, "hostname"), *port = jstr(t, "port"),
                       *pathname = jstr(t, "pathname"), *search = jstr(t, "search"), *hash = jstr(t, "hash"),
                       *origin = jstr(t, "origin");
            char tmp[64];
            AuBuf ob;
            au_buf_init(&ob);
#define CMP(field, got)                                                                                           \
    if (good && field && strcmp(field, got)) {                                                                    \
        good = false;                                                                                             \
        snprintf(reason, sizeof reason, #field " is \"%s\", expected \"%s\"", got, field);                        \
    }
            CMP(href, u.href);
            snprintf(tmp, sizeof tmp, "%s:", u.scheme);
            CMP(proto, tmp);
            CMP(user, u.username);
            CMP(pass_, u.password);
            CMP(hostname, u.host);
            snprintf(tmp, sizeof tmp, "%d", u.port);
            CMP(port, u.port >= 0 ? tmp : "");
            CMP(pathname, u.path);
            au_buf_clear(&ob);
            if (u.query && u.query[0]) {
                au_buf_putc(&ob, '?');
                au_buf_puts(&ob, u.query);
            }
            CMP(search, au_buf_cstr(&ob));
            au_buf_clear(&ob);
            if (u.fragment && u.fragment[0]) {
                au_buf_putc(&ob, '#');
                au_buf_puts(&ob, u.fragment);
            }
            CMP(hash, au_buf_cstr(&ob));
            if (origin) {
                AuOrigin o;
                au_url_origin(&u, &o);
                au_buf_clear(&ob);
                au_origin_serialize(&o, &ob);
                CMP(origin, au_buf_cstr(&ob));
                au_origin_free(&o);
            }
            au_buf_free(&ob);
#undef CMP
        }
        if (ok)
            au_url_free(&u);
        if (have_base)
            au_url_free(&base);
        if (good) {
            pass++;
            continue;
        }
        fail++;
        AuBuf key;
        au_buf_init(&key);
        escape(in->str, &key);
        au_buf_cstr(&key);
        /* Listed in the expected failures? (whole line match) */
        bool listed = false;
        if (expected) {
            for (char *line = expected; line && *line;) {
                char *nl = strchr(line, '\n');
                size_t ll = nl ? (size_t)(nl - line) : strlen(line);
                if (ll == key.len && !memcmp(line, key.data, ll))
                    listed = true;
                line = nl ? nl + 1 : NULL;
            }
        }
        if (listed) {
            expected_fail++;
        } else {
            unexpected++;
            printf("FAIL %s (base %s): %s\n", key.data, basev && basev->type == J_STR ? basev->str : "none", reason);
        }
        au_buf_free(&key);
    }
    printf("WPT urltestdata: %d passed, %d failed (%d expected, %d unexpected)\n", pass, fail, expected_fail,
           unexpected);
    json_free(root);
    free(data);
    free(expected);
    return unexpected ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--wpt"))
        return wpt(argv[2], argc >= 4 ? argv[3] : "");
    own_cases();
    components();
    punycode();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
