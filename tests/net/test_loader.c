/* Loader tests: data: URLs, mixed content, sniffing and nosniff, UTF-8
 * delivery of HTML documents, security state, downloads (adopted and
 * started with a range), and the AuPageFetcher adapter's inference of
 * navigations and subresources. */
#include "fakenet.h"
#include "net/cookies.h"
#include "net/loader.h"
#include "browser/page.h"

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

typedef struct R {
    bool responded, done, take_download;
    AuNetError err;
    char mime[64], encoding[32], url[256], filename[64];
    AuLoadSecurity security;
    bool attachment;
    AuBuf body;
    AuNetLoader *l;
    AuNetDownload *dl;
    AuDownloadSink sink;
} R;

static void r_response(void *ctx, uint32_t id, const AuLoadInfo *i)
{
    (void)id;
    R *r = ctx;
    r->responded = true;
    snprintf(r->mime, sizeof r->mime, "%s", i->mime);
    snprintf(r->encoding, sizeof r->encoding, "%s", i->encoding);
    r->security = i->security;
}
static void r_data(void *ctx, uint32_t id, const void *b, size_t n)
{
    (void)id;
    au_buf_append(&((R *)ctx)->body, b, n);
}
static void r_done(void *ctx, uint32_t id, AuNetError e, const AuLoadInfo *i)
{
    (void)id;
    R *r = ctx;
    r->done = true;
    r->err = e;
    snprintf(r->url, sizeof r->url, "%s", i->url);
    if (!r->responded) {
        snprintf(r->mime, sizeof r->mime, "%s", i->mime);
        snprintf(r->encoding, sizeof r->encoding, "%s", i->encoding);
    }
    r->security = i->security;
}

static bool sink_write(void *ctx, const void *b, size_t n)
{
    au_buf_append((AuBuf *)ctx, b, n);
    return true;
}

static bool r_download(void *ctx, uint32_t id, const AuLoadInfo *i)
{
    R *r = ctx;
    r->attachment = i->attachment;
    snprintf(r->filename, sizeof r->filename, "%s", i->filename);
    snprintf(r->mime, sizeof r->mime, "%s", i->mime);
    if (!r->take_download)
        return false;
    r->dl = au_net_download_adopt(r->l, id, &r->sink);
    return true;
}

static void load(FakeNet *f, AuNetLoader *l, R *r, const char *url, AuRequestDest dest, const char *doc)
{
    bool take = r->take_download;
    au_buf_free(&r->body);
    memset(r, 0, sizeof *r);
    r->take_download = take;
    au_buf_init(&r->body);
    r->l = l;
    AuLoadRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.dest = dest;
    rq.document_url = doc;
    rq.top_level_url = doc;
    rq.to_utf8 = true;
    AuLoadCallbacks cb = {r, r_response, r_data, r_done, r_download};
    au_net_loader_start(l, &rq, &cb);
    CHECK(!r->done && !r->responded, "%s: nothing delivered from start", url);
    for (int i = 0; i < 400 && !r->done; i++) {
        au_net_loader_poll(l);
        f->now += 10;
    }
}

static void test_data(void)
{
    FakeNet f;
    fake_init(&f);
    AuNet *net = au_net_new(&f.ops);
    AuNetLoader *l = au_net_loader_new(net);
    R r;
    memset(&r, 0, sizeof r);
    au_buf_init(&r.body);
    load(&f, l, &r, "data:text/plain,hello%20world", AU_DEST_IMAGE, NULL);
    CHECK(r.done && r.err == AU_NET_OK && r.body.len == 11 && !strcmp(r.mime, "text/plain"), "data: plain");
    load(&f, l, &r, "data:text/html;charset=windows-1252,%E9t%E9", AU_DEST_DOCUMENT, NULL);
    CHECK(r.done && r.err == AU_NET_OK && r.body.len == 8 && !memcmp(r.body.data, "\xEF\xBB\xBF\xC3\xA9t\xC3\xA9", 8),
          "data: HTML in windows-1252 delivered as UTF-8 with a BOM (%zu bytes)", r.body.len);
    load(&f, l, &r, "data:text/plain;base64,%%%", AU_DEST_IMAGE, NULL);
    CHECK(r.done && r.err == AU_NET_ERR_BAD_URL, "bad data URL: %s", au_net_error_name(r.err));
    load(&f, l, &r, "not a url", AU_DEST_IMAGE, NULL);
    CHECK(r.done && r.err == AU_NET_ERR_BAD_URL, "bad URL");
    au_buf_free(&r.body);
    au_net_loader_free(l);
    au_net_free(net);
    fake_free(&f);
}

static void test_mixed_and_types(void)
{
    FakeNet f;
    fake_init(&f);
    AuNet *net = au_net_new(&f.ops);
    AuNetLoader *l = au_net_loader_new(net);
    FakeServer *s = fake_server(&f, "m.test", 80); /* the blocked request never reaches it */
    R r;
    memset(&r, 0, sizeof r);
    au_buf_init(&r.body);
    load(&f, l, &r, "http://m.test/s.css", AU_DEST_STYLE, "https://secure.test/");
    CHECK(r.done && r.err == AU_NET_ERR_BLOCKED && s->nreq == 0, "mixed style blocked: %s", au_net_error_name(r.err));
    /* An http image from an https page goes out over https. */
    FakeServer *si = fake_server(&f, "img.test", 0); /* any port: catch the upgrade */
    fake_respond(si, "HTTP/1.1 200 OK\r\nContent-Type: image/gif\r\nContent-Length: 6\r\n\r\nGIF89a");
    load(&f, l, &r, "http://img.test/a.gif", AU_DEST_IMAGE, "https://secure.test/");
    CHECK(!strncmp(r.url, "https://img.test/", 17), "mixed image upgraded: %s", r.url);
    /* nosniff: a style sheet served as text/plain is refused. */
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nX-Content-Type-Options: nosniff\r\n"
                    "Content-Length: 1\r\n\r\nx");
    load(&f, l, &r, "http://m.test/t.css", AU_DEST_STYLE, "http://m.test/");
    CHECK(r.done && r.err == AU_NET_ERR_BLOCKED && r.body.len == 0, "nosniff style blocked: %s",
          au_net_error_name(r.err));
    /* Sniffing a document without a type. */
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 15\r\n\r\n<html><p>x</p>\n");
    load(&f, l, &r, "http://m.test/untyped", AU_DEST_DOCUMENT, NULL);
    CHECK(r.done && r.err == AU_NET_OK && !strcmp(r.mime, "text/html"), "sniffed text/html: %s", r.mime);
    CHECK(r.security == AU_LOAD_SECURITY_NOT_SECURE, "http is not secure");
    CHECK(r.body.len == 18 && !memcmp(r.body.data, "\xEF\xBB\xBF<html>", 9), "UTF-8 BOM added: %zu", r.body.len);
    /* A UTF-8 BOM already there is not doubled; windows-1252 via meta. */
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 6\r\n\r\n\xEF\xBB\xBFok!");
    load(&f, l, &r, "http://m.test/bom", AU_DEST_DOCUMENT, NULL);
    CHECK(r.body.len == 6 && !strcmp(r.encoding, "UTF-8"), "BOM kept once: %zu %s", r.body.len, r.encoding);
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 35\r\n\r\n"
                    "<meta charset=iso-8859-1><p>\x93q\x94</p>");
    load(&f, l, &r, "http://m.test/latin", AU_DEST_DOCUMENT, NULL);
    CHECK(!strcmp(r.encoding, "windows-1252") && strstr(au_buf_cstr(&r.body), "<p>\xE2\x80\x9Cq\xE2\x80\x9D</p>"),
          "windows-1252 quotes as UTF-8: %s", r.encoding);
    /* Other encodings pass through, named. */
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=Shift_JIS\r\nContent-Length: 4\r\n\r\n"
                    "\x82\xa0\x82\xa2");
    load(&f, l, &r, "http://m.test/sjis", AU_DEST_DOCUMENT, NULL);
    CHECK(!strcmp(r.encoding, "Shift_JIS") && r.body.len == 4, "Shift_JIS passed through: %s %zu", r.encoding,
          r.body.len);
    au_buf_free(&r.body);
    au_net_loader_free(l);
    au_net_free(net);
    fake_free(&f);
}

static void test_downloads(void)
{
    FakeNet f;
    fake_init(&f);
    AuNet *net = au_net_new(&f.ops);
    AuNetLoader *l = au_net_loader_new(net);
    FakeServer *s = fake_server(&f, "d.test", 80);
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\n"
                    "Content-Disposition: attachment; filename=\"../evil:name.zip\"\r\nContent-Length: 4\r\n\r\nPK..");
    AuBuf file;
    au_buf_init(&file);
    R r;
    memset(&r, 0, sizeof r);
    au_buf_init(&r.body);
    r.take_download = true;
    r.sink.ctx = &file;
    r.sink.write = sink_write;
    /* load() resets r but keeps take_download; restore the sink after. */
    {
        bool take = true;
        au_buf_free(&r.body);
        memset(&r, 0, sizeof r);
        au_buf_init(&r.body);
        r.take_download = take;
        r.l = l;
        r.sink.ctx = &file;
        r.sink.write = sink_write;
        AuLoadRequest rq;
        memset(&rq, 0, sizeof rq);
        rq.url = "http://d.test/file.zip";
        rq.dest = AU_DEST_DOCUMENT;
        AuLoadCallbacks cb = {&r, r_response, r_data, r_done, r_download};
        au_net_loader_start(l, &rq, &cb);
        for (int i = 0; i < 200; i++) {
            au_net_loader_poll(l);
            f.now += 10;
        }
    }
    CHECK(r.dl != NULL && r.attachment && !strcmp(r.filename, ".._evil_name.zip"), "download offered: %s",
          r.filename);
    CHECK(!r.done && r.body.len == 0, "no page callbacks after adoption");
    int64_t total = -1;
    bool done = false;
    char *err = NULL;
    int64_t got = r.dl ? au_net_download_poll(r.dl, &total, &done, &err) : 0;
    CHECK(done && !err && got == 4 && total == 4 && file.len == 4 && !memcmp(file.data, "PK..", 4),
          "adopted download complete: %lld/%lld", (long long)got, (long long)total);
    au_net_download_stop(r.dl);

    /* A download started with an offset asks for a range. */
    au_buf_clear(&file);
    fake_respond(s, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 10-14/15\r\nContent-Length: 5\r\n\r\nworld");
    AuDownloadSink sink = {&file, sink_write};
    AuNetDownload *d = au_net_download_start(l, "http://d.test/big.bin", 10, &sink, &err);
    for (int i = 0; i < 200; i++) {
        au_net_loader_poll(l);
        f.now += 10;
    }
    got = au_net_download_poll(d, &total, &done, &err);
    CHECK(done && !err && got == 15 && total == 15 && file.len == 5, "resumed download: %lld/%lld",
          (long long)got, (long long)total);
    CHECK(s->nreq >= 2 && strstr(s->requests[s->nreq - 1], "\r\nRange: bytes=10-\r\n"), "Range header");
    au_net_download_stop(d);
    /* A server that ignores the range. */
    fake_respond(s, "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc");
    d = au_net_download_start(l, "http://d.test/norange", 10, &sink, &err);
    for (int i = 0; i < 200; i++) {
        au_net_loader_poll(l);
        f.now += 10;
    }
    got = au_net_download_poll(d, &total, &done, &err);
    CHECK(err != NULL, "resume refused when the server ignores Range");
    au_free(err);
    au_net_download_stop(d);
    CHECK(au_net_download_start(l, "ftp://x/", 0, &sink, &err) == NULL && err, "only http(s)");
    au_free(err);
    au_buf_free(&file);
    au_buf_free(&r.body);
    au_net_loader_free(l);
    au_net_free(net);
    fake_free(&f);
}

/* ---- The page fetcher adapter ---- */
typedef struct S {
    AuBuf body;
    bool done, ok, responded;
    char ct[64], url[256], resp_url[256];
    AuFetchError err;
    AuSecurity security;
} S;

static void s_response(void *ctx, uint32_t id, const AuFetchResponse *r)
{
    (void)id;
    S *s = ctx;
    s->responded = true;
    s->security = r->security;
    snprintf(s->resp_url, sizeof s->resp_url, "%s", r->url);
}
static void s_data(void *ctx, uint32_t id, const void *b, size_t n)
{
    (void)id;
    au_buf_append(&((S *)ctx)->body, b, n);
}
static void s_done(void *ctx, uint32_t id, bool ok, const AuFetchInfo *i)
{
    (void)id;
    S *s = ctx;
    s->done = true;
    s->ok = ok;
    s->err = i ? i->error : AU_FETCH_OK;
    snprintf(s->ct, sizeof s->ct, "%s", i && i->content_type ? i->content_type : "");
    snprintf(s->url, sizeof s->url, "%s", i && i->url ? i->url : "");
}

static void run_fetcher(FakeNet *f, AuPageFetcher *fe, int rounds)
{
    for (int i = 0; i < rounds; i++) {
        fe->poll(fe->ctx);
        f->now += 10;
    }
}

static void test_adapter(void)
{
    FakeNet f;
    fake_init(&f);
    AuNet *net = au_net_new(&f.ops);
    AuCookieJar *jar = au_cookie_jar_new();
    au_net_set_cookie_jar(net, jar);
    AuNetLoader *l = au_net_loader_new(net);
    AuNetPageFetcher *pf = au_net_page_fetcher_new(l);
    AuPageFetcher fe;
    au_net_page_fetcher_interface(pf, &fe);
    FakeServer *site = fake_server(&f, "site.test", 80);
    FakeServer *tracker = fake_server(&f, "tracker.test", 80);
    fake_respond(site, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nSet-Cookie: s=1\r\nContent-Length: 13\r\n\r\n"
                       "<p>page</p>\r\n");
    fake_respond(tracker, "HTTP/1.1 200 OK\r\nContent-Type: image/gif\r\nSet-Cookie: t=1; SameSite=None; Secure\r\n"
                          "Content-Length: 6\r\n\r\nGIF89a");
    S a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    au_buf_init(&a.body);
    au_buf_init(&b.body);
    AuFetchSink sa = {&a, s_response, s_data, s_done, NULL}, sb = {&b, s_response, s_data, s_done, NULL};
    AuFetchRequest ra = {0}, rb = {0};
    ra.url = "http://site.test/";
    ra.kind = AU_FETCH_DOCUMENT;
    ra.user_activated = true;
    uint32_t ida = fe.start(fe.ctx, &ra, &sa);
    /* The page's image: a third-party subresource of the page. */
    rb.url = "http://tracker.test/p.gif";
    rb.kind = AU_FETCH_IMAGE;
    rb.document_url = "http://site.test/";
    rb.top_level_url = "http://site.test/";
    uint32_t idb = fe.start(fe.ctx, &rb, &sb);
    CHECK(ida && idb && ida != idb, "ids");
    run_fetcher(&f, &fe, 200);
    CHECK(a.done && a.ok && a.responded && !strcmp(a.ct, "text/html") && a.body.len == 16, "page: %s %zu", a.ct,
          a.body.len);
    CHECK(a.security == AU_SECURITY_NOT_SECURE, "http page: not secure");
    CHECK(b.done && b.ok && !strcmp(b.ct, "image/gif"), "image: %s", b.ct);
    CHECK(tracker->nreq == 1 && strstr(tracker->requests[0], "Referer: http://site.test/"),
          "subresource has the page as referrer");
    CHECK(au_cookie_jar_count(jar) == 1, "third-party cookie not stored: %zu cookies", au_cookie_jar_count(jar));
    /* A navigation by the user sends no referrer, even right after another
     * load (no guessing from timing). */
    fake_respond(tracker, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 2\r\n\r\nhi");
    S c;
    memset(&c, 0, sizeof c);
    au_buf_init(&c.body);
    AuFetchSink sc = {&c, s_response, s_data, s_done, NULL};
    AuFetchRequest rc = {0};
    rc.url = "http://tracker.test/";
    rc.kind = AU_FETCH_DOCUMENT;
    fe.start(fe.ctx, &rc, &sc);
    run_fetcher(&f, &fe, 200);
    CHECK(c.done && c.ok, "second navigation");
    CHECK(tracker->nreq == 2 && !strstr(tracker->requests[1], "Referer:"), "a navigation sends no referrer");
    CHECK(strstr(tracker->requests[1], "Upgrade-Insecure-Requests: 1") != NULL, "navigation mode");
    /* Reload: revalidate. */
    fake_respond(site, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nzz");
    S r2;
    memset(&r2, 0, sizeof r2);
    au_buf_init(&r2.body);
    AuFetchSink sr = {&r2, s_response, s_data, s_done, NULL};
    AuFetchRequest rr = {0};
    rr.url = "http://site.test/";
    rr.kind = AU_FETCH_DOCUMENT;
    rr.cache = AU_FETCH_CACHE_REVALIDATE;
    fe.start(fe.ctx, &rr, &sr);
    run_fetcher(&f, &fe, 200);
    CHECK(r2.done && site->nreq == 2 && strstr(site->requests[1], "Cache-Control: max-age=0"), "reload revalidates");
    /* Status: a fetch that has not started its connection. */
    fake_respond(site, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nzz");
    S d;
    memset(&d, 0, sizeof d);
    au_buf_init(&d.body);
    AuFetchSink sd = {&d, s_response, s_data, s_done, NULL};
    AuFetchRequest rd = {0};
    rd.url = "http://site.test/x";
    rd.kind = AU_FETCH_DOCUMENT;
    uint32_t idd = fe.start(fe.ctx, &rd, &sd);
    AuFetchStatus st;
    CHECK(fe.status(fe.ctx, idd, &st) && st.phase == AU_FETCH_PHASE_QUEUED, "status before the pump: %d", st.phase);
    /* Cancel: no callbacks. */
    fe.cancel(fe.ctx, idd);
    run_fetcher(&f, &fe, 100);
    CHECK(!d.done && d.body.len == 0, "cancelled fetch is silent");
    /* Errors arrive as AuFetchError. */
    FakeServer *gone = fake_server(&f, "gone.test", 80);
    gone->dns_fail = true;
    S g;
    memset(&g, 0, sizeof g);
    au_buf_init(&g.body);
    AuFetchSink sg = {&g, s_response, s_data, s_done, NULL};
    AuFetchRequest rg = {0};
    rg.url = "http://gone.test/";
    rg.kind = AU_FETCH_DOCUMENT;
    fe.start(fe.ctx, &rg, &sg);
    run_fetcher(&f, &fe, 100);
    CHECK(g.done && !g.ok && g.err == AU_FETCH_ERR_DNS, "DNS error: %d", g.err);
    au_buf_free(&a.body);
    au_buf_free(&b.body);
    au_buf_free(&c.body);
    au_buf_free(&r2.body);
    au_buf_free(&d.body);
    au_buf_free(&g.body);
    au_net_page_fetcher_free(pf);
    au_net_loader_free(l);
    au_net_free(net);
    au_cookie_jar_free(jar);
    fake_free(&f);
}

int main(void)
{
    test_data();
    test_mixed_and_types();
    test_downloads();
    test_adapter();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
