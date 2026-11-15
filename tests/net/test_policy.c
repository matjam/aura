/* Policy helper tests: data: URLs, HSTS, the Public Suffix List, referrer
 * policy, Fetch Metadata, mixed content, MIME sniffing, and encoding
 * labels and detection. */
#include "net/dataurl.h"
#include "net/hsts.h"
#include "net/net.h"
#include "net/policy.h"
#include "net/psl.h"
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

/* ---- data: URLs ---- */
static void data_case(const char *url, bool ok, const char *mime, const char *charset, const char *body,
                      size_t body_len)
{
    AuDataUrl d;
    au_data_url_init(&d);
    bool got = au_data_url_decode(url, &d);
    CHECK(got == ok, "%s: decode %d", url, got);
    if (got && ok) {
        CHECK(!strcmp(au_buf_cstr(&d.mime), mime), "%s: mime %s, want %s", url, au_buf_cstr(&d.mime), mime);
        CHECK(!strcmp(au_buf_cstr(&d.charset), charset), "%s: charset %s, want %s", url, au_buf_cstr(&d.charset),
              charset);
        CHECK(d.body.len == body_len && (!body_len || !memcmp(d.body.data, body, body_len)), "%s: body (%zu bytes)",
              url, d.body.len);
    }
    au_data_url_free(&d);
}

static void test_data_urls(void)
{
    data_case("data:,Hello%2C%20World!", true, "text/plain", "US-ASCII", "Hello, World!", 13);
    data_case("data:text/plain;base64,SGVsbG8sIFdvcmxkIQ==", true, "text/plain", "", "Hello, World!", 13);
    data_case("data:text/html,%3Ch1%3EHello%3C%2Fh1%3E", true, "text/html", "", "<h1>Hello</h1>", 14);
    data_case("data:;charset=UTF-8,x", true, "text/plain", "UTF-8", "x", 1);
    data_case("DATA:TEXT/HTML;CHARSET=utf-8,x", true, "text/html", "utf-8", "x", 1);
    data_case("data:text/plain ; base64 ,SGk=", true, "text/plain", "", "Hi", 2);
    data_case("data:image/png;BASE64,iVBO", true, "image/png", "", "\x89PN", 3);
    data_case("data:text/plain;base64,SGk", true, "text/plain", "", "Hi", 2); /* forgiving: no padding */
    data_case("data:text/plain;base64, S G k = ", true, "text/plain", "", "Hi", 2);
    data_case("data:text/plain;base64,S", false, "", "", "", 0);   /* one leftover sextet */
    data_case("data:text/plain;base64,SGk=a", false, "", "", "", 0); /* data after padding */
    data_case("data:text/plain;base64,%", false, "", "", "", 0);
    data_case("data:nonsense,x", true, "text/plain", "US-ASCII", "x", 1);
    data_case("data:text/plain", false, "", "", "", 0); /* no comma */
    data_case("data:,a#frag", true, "text/plain", "US-ASCII", "a", 1);
    data_case("data:,%00%ff", true, "text/plain", "US-ASCII", "\0\xff", 2);
    data_case("data:,%zz", true, "text/plain", "US-ASCII", "%zz", 3);
    /* ";base64" not at the end: not base64. */
    data_case("data:text/plain;base64;x=y,SGk=", true, "text/plain", "", "SGk=", 4);
    data_case("http://example.com/,x", false, "", "", "", 0);
    data_case("data:text/plain;charset=\"utf-8\",x", true, "text/plain", "utf-8", "x", 1);
}

/* ---- HSTS ---- */
static void test_hsts(void)
{
    int64_t ma;
    bool sub;
    CHECK(au_hsts_parse("max-age=31536000", &ma, &sub) && ma == 31536000 && !sub, "basic");
    CHECK(au_hsts_parse("max-age=\"100\"; includeSubDomains", &ma, &sub) && ma == 100 && sub, "quoted, sub");
    CHECK(au_hsts_parse("  INCLUDESUBDOMAINS ;MAX-AGE=5 ; preload", &ma, &sub) && ma == 5 && sub, "case, order");
    CHECK(!au_hsts_parse("includeSubDomains", &ma, &sub), "no max-age");
    CHECK(!au_hsts_parse("max-age=1; max-age=2", &ma, &sub), "duplicate directive");
    CHECK(!au_hsts_parse("max-age=abc", &ma, &sub), "bad number");
    CHECK(!au_hsts_parse("max-age=-1", &ma, &sub), "negative");
    CHECK(au_hsts_parse("max-age=99999999999999999999", &ma, &sub) && ma > 0, "huge max-age clamps");

    AuHsts *h = au_hsts_new();
    int64_t now = 1800000000;
    CHECK(au_hsts_process(h, "a.example", "max-age=100", now), "process");
    CHECK(au_hsts_is_known(h, "a.example", now + 99), "known");
    CHECK(!au_hsts_is_known(h, "a.example", now + 101), "expired");
    CHECK(!au_hsts_is_known(h, "b.a.example", now), "no subdomains without includeSubDomains");
    au_hsts_process(h, "s.example", "max-age=100; includeSubDomains", now);
    CHECK(au_hsts_is_known(h, "x.y.s.example", now), "includeSubDomains");
    CHECK(!au_hsts_is_known(h, "ss.example", now), "not a suffix match");
    au_hsts_process(h, "s.example", "max-age=0", now);
    CHECK(!au_hsts_is_known(h, "s.example", now), "max-age=0 removes");
    CHECK(!au_hsts_process(h, "192.0.2.1", "max-age=100", now) || !au_hsts_is_known(h, "192.0.2.1", now),
          "IP addresses never");
    /* Preload: the acceptance sites. */
    CHECK(au_hsts_is_known(h, "github.com", now) && au_hsts_is_known(h, "gist.github.com", now), "preload github");
    CHECK(au_hsts_is_known(h, "en.wikipedia.org", now), "preload wikipedia");
    CHECK(au_hsts_is_known(h, "old.reddit.com", now), "preload reddit");
    CHECK(au_hsts_preloaded("news.ycombinator.com") && au_hsts_preloaded("html.duckduckgo.com"), "preload others");
    CHECK(!au_hsts_preloaded("example.com"), "not preloaded");
    /* A preloaded host stays known even after max-age=0. */
    au_hsts_process(h, "github.com", "max-age=0", now);
    CHECK(au_hsts_is_known(h, "github.com", now), "preload not removable");

    /* Persistence round trip. */
    AuBuf b;
    au_buf_init(&b);
    CHECK(au_hsts_dirty(h), "dirty");
    /* (Asking about a later time dropped a.example as expired.) */
    au_hsts_process(h, "r.example", "max-age=100", now);
    au_hsts_serialize(h, &b, now);
    AuHsts *h2 = au_hsts_new();
    CHECK(au_hsts_parse_file(h2, au_buf_cstr(&b), b.len, now), "parse back");
    CHECK(au_hsts_is_known(h2, "r.example", now) && !au_hsts_is_known(h2, "r.example", now + 200),
          "round trip keeps expiry: %s", au_buf_cstr(&b));
    AuHsts *h3 = au_hsts_new();
    const char *bad = "# Aura HSTS 1\nok.example\t1900000000\t0\nbroken\nx\tnan\t1\n";
    CHECK(!au_hsts_parse_file(h3, bad, strlen(bad), now), "damaged reported");
    CHECK(au_hsts_is_known(h3, "ok.example", now), "good line kept");
    au_buf_free(&b);
    au_hsts_free(h);
    au_hsts_free(h2);
    au_hsts_free(h3);
}

/* ---- PSL ---- */
static void reg_case(const char *host, const char *want)
{
    const char *d;
    size_t n;
    bool ok = au_psl_registrable(host, &d, &n);
    if (!want)
        CHECK(!ok, "%s: no registrable domain, got %.*s", host, ok ? (int)n : 0, ok ? d : "");
    else
        CHECK(ok && strlen(want) == n && !memcmp(d, want, n), "%s: got %.*s, want %s", host, ok ? (int)n : 4,
              ok ? d : "none", want);
}

static void test_psl(void)
{
    /* From the list's own test cases (test_psl.txt), lower case. */
    reg_case("com", NULL);
    reg_case("example.com", "example.com");
    reg_case("www.example.com", "example.com");
    reg_case("uk.com", NULL);
    reg_case("example.uk.com", "example.uk.com");
    reg_case("b.example.uk.com", "example.uk.com");
    reg_case("a.b.example.uk.com", "example.uk.com");
    reg_case("test.ac", "test.ac");
    reg_case("mm", NULL);
    reg_case("c.mm", NULL);
    reg_case("b.c.mm", "b.c.mm");
    reg_case("a.b.c.mm", "b.c.mm");
    reg_case("jp", NULL);
    reg_case("test.jp", "test.jp");
    reg_case("www.test.jp", "test.jp");
    reg_case("ac.jp", NULL);
    reg_case("test.ac.jp", "test.ac.jp");
    reg_case("kyoto.jp", NULL);
    reg_case("test.kyoto.jp", "test.kyoto.jp");
    reg_case("ide.kyoto.jp", NULL);
    reg_case("b.ide.kyoto.jp", "b.ide.kyoto.jp");
    reg_case("c.kobe.jp", NULL);
    reg_case("b.c.kobe.jp", "b.c.kobe.jp");
    reg_case("city.kobe.jp", "city.kobe.jp");
    reg_case("www.city.kobe.jp", "city.kobe.jp");
    reg_case("ck", NULL);
    reg_case("test.ck", NULL);
    reg_case("b.test.ck", "b.test.ck");
    reg_case("www.ck", "www.ck");
    reg_case("www.www.ck", "www.ck");
    reg_case("us", NULL);
    reg_case("test.us", "test.us");
    reg_case("ak.us", NULL);
    reg_case("test.ak.us", "test.ak.us");
    reg_case("k12.ak.us", NULL);
    reg_case("test.k12.ak.us", "test.k12.ak.us");
    reg_case("xn--85x722f.com.cn", "xn--85x722f.com.cn");
    reg_case("xn--55qx5d.cn", NULL);
    reg_case("www.xn--85x722f.xn--55qx5d.cn", "xn--85x722f.xn--55qx5d.cn");
    reg_case("unlisted-tld-xyz", NULL);
    reg_case("example.unlisted-tld-xyz", "example.unlisted-tld-xyz");
    reg_case("github.io", NULL);
    reg_case("user.github.io", "user.github.io");
    reg_case("192.168.0.1", NULL);
    CHECK(au_psl_is_public_suffix("co.uk") && !au_psl_is_public_suffix("bbc.co.uk"), "is_public_suffix");
    CHECK(au_same_registrable("a.example.co.uk", "b.example.co.uk"), "same registrable");
    CHECK(!au_same_registrable("a.co.uk", "b.co.uk"), "different registrable");
    AuUrl a, b;
    au_url_parse(&a, "https://www.example.com/", NULL);
    au_url_parse(&b, "https://cdn.example.com:8443/x", NULL);
    CHECK(au_same_site(&a, &b), "same site across hosts and ports");
    au_url_free(&b);
    au_url_parse(&b, "http://www.example.com/", NULL);
    CHECK(!au_same_site(&a, &b), "schemeful: http vs https");
    au_url_free(&a);
    au_url_free(&b);
}

/* ---- Referrer policy ---- */
static void ref_case(const char *from, const char *to, AuReferrerPolicy p, const char *want)
{
    AuUrl t;
    au_url_parse(&t, to, NULL);
    AuBuf b;
    au_buf_init(&b);
    bool sent = au_referrer_compute(from, &t, p, &b);
    if (!want)
        CHECK(!sent, "%s -> %s (policy %d): none expected, got %s", from, to, (int)p, au_buf_cstr(&b));
    else
        CHECK(sent && !strcmp(au_buf_cstr(&b), want), "%s -> %s (policy %d): got %s, want %s", from, to, (int)p,
              sent ? au_buf_cstr(&b) : "none", want);
    au_buf_free(&b);
    au_url_free(&t);
}

static void test_referrer(void)
{
    const char *page = "https://user:pw@a.example/path/page?q=1#frag";
    /* Default: strict-origin-when-cross-origin. */
    ref_case(page, "https://a.example/other", AU_RP_DEFAULT, "https://a.example/path/page?q=1");
    ref_case(page, "https://b.example/", AU_RP_DEFAULT, "https://a.example/");
    ref_case(page, "http://a.example/", AU_RP_DEFAULT, NULL);
    ref_case(page, "https://a.example/", AU_RP_NO_REFERRER, NULL);
    ref_case(page, "http://b.example/", AU_RP_NO_REFERRER_WHEN_DOWNGRADE, NULL);
    ref_case(page, "https://b.example/", AU_RP_NO_REFERRER_WHEN_DOWNGRADE, "https://a.example/path/page?q=1");
    ref_case(page, "https://b.example/", AU_RP_SAME_ORIGIN, NULL);
    ref_case(page, "https://a.example/", AU_RP_SAME_ORIGIN, "https://a.example/path/page?q=1");
    ref_case(page, "http://b.example/", AU_RP_ORIGIN, "https://a.example/");
    ref_case(page, "http://b.example/", AU_RP_STRICT_ORIGIN, NULL);
    ref_case(page, "https://b.example/", AU_RP_ORIGIN_WHEN_CROSS_ORIGIN, "https://a.example/");
    ref_case(page, "http://b.example/", AU_RP_UNSAFE_URL, "https://a.example/path/page?q=1");
    ref_case("about:blank", "https://b.example/", AU_RP_UNSAFE_URL, NULL);
    ref_case("data:text/html,x", "https://b.example/", AU_RP_UNSAFE_URL, NULL);
    ref_case("http://a.example/x", "http://b.example/", AU_RP_DEFAULT, "http://a.example/");
    CHECK(au_referrer_policy_parse("no-referrer, bogus", AU_RP_DEFAULT) == AU_RP_NO_REFERRER, "parse last known");
    CHECK(au_referrer_policy_parse("unsafe-url, origin", AU_RP_DEFAULT) == AU_RP_ORIGIN, "last wins");
    CHECK(au_referrer_policy_parse("bogus", AU_RP_SAME_ORIGIN) == AU_RP_SAME_ORIGIN, "unknown keeps current");
    CHECK(au_referrer_policy_parse("NO-REFERRER", AU_RP_DEFAULT) == AU_RP_NO_REFERRER, "case-insensitive");

    AuUrl t;
    au_url_parse(&t, "https://a.example/x", NULL);
    CHECK(!strcmp(au_sec_fetch_site(NULL, &t), "none"), "site none");
    CHECK(!strcmp(au_sec_fetch_site("https://a.example/y", &t), "same-origin"), "same-origin");
    CHECK(!strcmp(au_sec_fetch_site("https://b.a.example/", &t), "same-site"), "same-site");
    CHECK(!strcmp(au_sec_fetch_site("https://b.example/", &t), "cross-site"), "cross-site");
    CHECK(!strcmp(au_sec_fetch_site("http://a.example/", &t), "cross-site"), "schemeful cross-site");
    CHECK(!strcmp(au_sec_fetch_dest(AU_DEST_IMAGE), "image") && !strcmp(au_sec_fetch_mode(AU_MODE_NAVIGATE),
                                                                        "navigate"),
          "dest/mode names");
    CHECK(au_url_trustworthy(&t), "https trustworthy");
    au_url_free(&t);
    au_url_parse(&t, "http://localhost:8080/", NULL);
    CHECK(au_url_trustworthy(&t), "localhost trustworthy");
    au_url_free(&t);
    au_url_parse(&t, "http://127.0.0.1/", NULL);
    CHECK(au_url_trustworthy(&t), "loopback trustworthy");
    au_url_free(&t);
    au_url_parse(&t, "http://example.com/", NULL);
    CHECK(!au_url_trustworthy(&t), "http not trustworthy");
    au_url_free(&t);
}

/* ---- Mixed content ---- */
static void test_mixed(void)
{
    AuUrl doc, img, js, sec;
    au_url_parse(&doc, "https://a.example/", NULL);
    au_url_parse(&img, "http://b.example/i.png", NULL);
    au_url_parse(&js, "http://b.example/s.js", NULL);
    au_url_parse(&sec, "https://b.example/s.js", NULL);
    CHECK(au_mixed_classify(&doc, &img, AU_DEST_IMAGE) == AU_MIXED_UPGRADEABLE, "image upgradeable");
    CHECK(au_mixed_classify(&doc, &js, AU_DEST_SCRIPT) == AU_MIXED_BLOCKABLE, "script blockable");
    CHECK(au_mixed_classify(&doc, &js, AU_DEST_STYLE) == AU_MIXED_BLOCKABLE, "style blockable");
    CHECK(au_mixed_classify(&doc, &sec, AU_DEST_SCRIPT) == AU_MIXED_NONE, "https not mixed");
    AuUrl http_doc;
    au_url_parse(&http_doc, "http://a.example/", NULL);
    CHECK(au_mixed_classify(&http_doc, &js, AU_DEST_SCRIPT) == AU_MIXED_NONE, "insecure document");
    AuUrl lh;
    au_url_parse(&lh, "http://localhost/x.js", NULL);
    CHECK(au_mixed_classify(&doc, &lh, AU_DEST_SCRIPT) == AU_MIXED_NONE, "localhost is trustworthy");
    au_url_upgrade_to_https(&img);
    CHECK(!strcmp(img.href, "https://b.example/i.png"), "upgrade: %s", img.href);
    AuUrl p;
    au_url_parse(&p, "http://b.example:8080/", NULL);
    au_url_upgrade_to_https(&p);
    CHECK(!strcmp(p.href, "https://b.example:8080/"), "upgrade keeps other port: %s", p.href);
    au_url_free(&doc);
    au_url_free(&img);
    au_url_free(&js);
    au_url_free(&sec);
    au_url_free(&http_doc);
    au_url_free(&lh);
    au_url_free(&p);
}

/* ---- MIME sniffing ---- */
#define SNIFF(supplied, nosniff, bytes, dest) \
    au_mime_sniff(supplied, nosniff, (const uint8_t *)(bytes), sizeof(bytes) - 1, dest)

static void test_sniff(void)
{
    CHECK(!strcmp(SNIFF("", false, "<!DOCTYPE html><p>", AU_DEST_DOCUMENT), "text/html"), "html doctype");
    CHECK(!strcmp(SNIFF("", false, "  \n<html>", AU_DEST_DOCUMENT), "text/html"), "leading space");
    CHECK(!strcmp(SNIFF("", false, "<!-- x -->", AU_DEST_DOCUMENT), "text/html"), "comment");
    CHECK(!strcmp(SNIFF("", false, "%PDF-1.4", AU_DEST_DOCUMENT), "application/pdf"), "pdf");
    CHECK(!strcmp(SNIFF("", false, "\x89PNG\r\n\x1a\n", AU_DEST_DOCUMENT), "image/png"), "png");
    CHECK(!strcmp(SNIFF("", false, "GIF89a", AU_DEST_DOCUMENT), "image/gif"), "gif");
    CHECK(!strcmp(SNIFF("", false, "\xff\xd8\xff", AU_DEST_DOCUMENT), "image/jpeg"), "jpeg");
    CHECK(!strcmp(SNIFF("", false, "plain words", AU_DEST_DOCUMENT), "text/plain"), "plain text");
    CHECK(!strcmp(SNIFF("", false, "\x00\x01\x02\x03", AU_DEST_DOCUMENT), "application/octet-stream"), "binary");
    CHECK(!strcmp(SNIFF("", false, "\xef\xbb\xbfhello", AU_DEST_DOCUMENT), "text/plain"), "UTF-8 BOM text");
    CHECK(!strcmp(SNIFF("unknown/unknown", false, "<html>", AU_DEST_DOCUMENT), "text/html"), "unknown/unknown");
    CHECK(!strcmp(SNIFF("application/unknown", false, "<html>", AU_DEST_DOCUMENT), "text/html"),
          "application/unknown");
    CHECK(!strcmp(SNIFF("*/*", false, "<html>", AU_DEST_DOCUMENT), "text/html"), "*/*");
    CHECK(!strcmp(SNIFF("text/plain", false, "<html>", AU_DEST_DOCUMENT), "text/plain"),
          "text/plain is not upgraded to html");
    CHECK(!strcmp(SNIFF("", true, "<html>", AU_DEST_DOCUMENT), "text/plain") ||
              !strcmp(SNIFF("", true, "<html>", AU_DEST_DOCUMENT), "application/octet-stream") ||
              !strcmp(SNIFF("", true, "<html>", AU_DEST_DOCUMENT), "text/html"),
          "nosniff without type");
    CHECK(!strcmp(SNIFF("text/html", true, "%PDF-", AU_DEST_DOCUMENT), "text/html"), "nosniff keeps type");
    CHECK(!strcmp(SNIFF("image/png", false, "GIF89a", AU_DEST_IMAGE), "image/gif"), "image sniffed by signature");
    CHECK(!strcmp(SNIFF("text/css", false, "<html>", AU_DEST_STYLE), "text/css"), "style never sniffs");
    CHECK(au_mime_allowed("text/css", true, AU_DEST_STYLE), "css allowed");
    CHECK(!au_mime_allowed("text/plain", true, AU_DEST_STYLE), "nosniff blocks text/plain style");
    CHECK(au_mime_allowed("text/plain", false, AU_DEST_STYLE), "without nosniff: allowed");
    CHECK(!au_mime_allowed("text/html", true, AU_DEST_SCRIPT), "nosniff blocks html script");
    CHECK(au_mime_allowed("application/javascript", true, AU_DEST_SCRIPT), "js allowed");
    CHECK(au_mime_is_javascript("text/ecmascript") && au_mime_is_javascript("application/x-javascript") &&
              !au_mime_is_javascript("text/plain") && !au_mime_is_javascript("application/json"),
          "javascript types");
}

/* ---- Encodings ---- */
static void test_encodings(void)
{
    struct {
        const char *label, *name;
    } cases[] = {
        {"utf-8", "UTF-8"},           {"UTF8", "UTF-8"},
        {"unicode-1-1-utf-8", "UTF-8"}, {"latin1", "windows-1252"},
        {"ISO-8859-1", "windows-1252"}, {"us-ascii", "windows-1252"},
        {"ascii", "windows-1252"},    {"cp1252", "windows-1252"},
        {"iso-8859-2", "ISO-8859-2"}, {"shift_jis", "Shift_JIS"},
        {"sjis", "Shift_JIS"},        {"euc-jp", "EUC-JP"},
        {"gbk", "GBK"},               {"gb2312", "GBK"},
        {"big5", "Big5"},             {"koi8-r", "KOI8-R"},
        {"utf-16", "UTF-16LE"},       {"utf-16be", "UTF-16BE"},
        {"x-user-defined", "x-user-defined"}, {"iso-2022-kr", "replacement"},
        {"windows-874", "windows-874"}, {"tis-620", "windows-874"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const char *n = au_encoding_label(cases[i].label, strlen(cases[i].label));
        CHECK(n && !strcmp(n, cases[i].name), "label %s: %s, want %s", cases[i].label, n ? n : "NULL",
              cases[i].name);
    }
    CHECK(au_encoding_label(" \tutf-8\n ", 9) && !strcmp(au_encoding_label(" \tutf-8\n ", 9), "UTF-8"),
          "label whitespace trimmed");
    /* The whole table: 228 labels, strictly sorted, each found. */
    size_t nl = au_encoding_label_count();
    CHECK(nl == 228, "label count %zu", nl);
    const char *prev = "";
    for (size_t i = 0; i < nl; i++) {
        const char *name, *l = au_encoding_label_at(i, &name);
        CHECK(strcmp(prev, l) < 0, "table order at %s", l);
        const char *got = au_encoding_label(l, strlen(l));
        CHECK(got == name, "lookup %s", l);
        prev = l;
    }
    CHECK(!au_encoding_label("utf-7", 5), "utf-7 unknown");
    CHECK(!au_encoding_label("", 0), "empty unknown");

    const uint8_t bom8[] = "\xef\xbb\xbf<meta charset=windows-1252>";
    CHECK(!strcmp(au_detect_encoding(bom8, sizeof bom8 - 1, "iso-8859-1"), "UTF-8"), "BOM wins");
    const uint8_t bom16[] = "\xff\xfe<\0h\0";
    CHECK(!strcmp(au_detect_encoding(bom16, sizeof bom16 - 1, NULL), "UTF-16LE"), "UTF-16LE BOM");
    const uint8_t meta[] = "<html><head><meta charset=\"utf-8\"></head>";
    CHECK(!strcmp(au_detect_encoding(meta, sizeof meta - 1, "ISO-8859-1"), "windows-1252"),
          "Content-Type beats meta");
    CHECK(!strcmp(au_detect_encoding(meta, sizeof meta - 1, NULL), "UTF-8"), "meta charset");
    CHECK(!strcmp(au_detect_encoding(meta, sizeof meta - 1, "bogus"), "UTF-8"), "unknown charset falls to meta");
    const uint8_t http_equiv[] = "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=shift_jis\">";
    CHECK(!strcmp(au_detect_encoding(http_equiv, sizeof http_equiv - 1, NULL), "Shift_JIS"), "http-equiv");
    const uint8_t none[] = "<p>hello</p>";
    CHECK(!strcmp(au_detect_encoding(none, sizeof none - 1, NULL), "windows-1252"), "default windows-1252");
    const uint8_t m16[] = "<meta charset=utf-16>";
    CHECK(!strcmp(au_detect_encoding(m16, sizeof m16 - 1, NULL), "UTF-8"), "meta utf-16 means UTF-8");
    const uint8_t xud[] = "<meta charset=x-user-defined>";
    CHECK(!strcmp(au_detect_encoding(xud, sizeof xud - 1, NULL), "windows-1252"),
          "meta x-user-defined means windows-1252");
}

int main(void)
{
    test_data_urls();
    test_hsts();
    test_psl();
    test_referrer();
    test_mixed();
    test_sniff();
    test_encodings();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
