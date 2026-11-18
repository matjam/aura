/* WebSocket (src/net/websocket.c and the tunnel in src/net/http.c): the
 * RFC 6455 handshake values, frames fed whole, split at every point and
 * one byte at a time, every protocol error the parser reports, masking,
 * and the handshake and tunnel over the fake network. */
#include "net/websocket.h"

#include "fakenet.h"

#include <stdio.h>
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

/* What the callbacks saw: "T text;" "B n;" "P n;" "O n;" "C code reason;"
 * "E code;". */
static void on_message(void *ctx, int op, const uint8_t *d, size_t n)
{
    AuBuf *b = ctx;
    if (op == AU_WS_TEXT) {
        au_buf_puts(b, "T ");
        au_buf_append(b, d, n);
    } else {
        au_buf_printf(b, "B %u", (unsigned)n);
    }
    au_buf_putc(b, ';');
}
static void on_control(void *ctx, int op, const uint8_t *d, size_t n)
{
    (void)d;
    au_buf_printf(ctx, "%c %u;", op == AU_WS_PING ? 'P' : 'O', (unsigned)n);
}
static void on_close(void *ctx, uint16_t code, const char *r, size_t n)
{
    AuBuf *b = ctx;
    au_buf_printf(b, "C %u ", code);
    au_buf_append(b, r, n);
    au_buf_putc(b, ';');
}
static void on_error(void *ctx, uint16_t code, const char *why)
{
    (void)why;
    au_buf_printf(ctx, "E %u;", code);
}

/* Feeds the bytes whole, split in two at every point and one at a time,
 * and checks that each gives want. */
static void expect(const char *name, const uint8_t *s, size_t n, const char *want)
{
    for (size_t mode = 0; mode <= n + 1; mode++) {
        AuBuf out;
        au_buf_init(&out);
        AuWsParser p;
        au_ws_parser_init(&p, 1024);
        AuWsCallbacks cb = {&out, on_message, on_control, on_close, on_error};
        if (mode == 0) {
            au_ws_feed(&p, s, n, &cb);
        } else if (mode <= n) {
            au_ws_feed(&p, s, mode - 1, &cb);
            au_ws_feed(&p, s + mode - 1, n - (mode - 1), &cb);
        } else {
            for (size_t i = 0; i < n; i++)
                au_ws_feed(&p, s + i, 1, &cb);
        }
        CHECK(!strcmp(au_buf_cstr(&out), want), "%s (mode %u): got \"%s\", want \"%s\"", name, (unsigned)mode,
              au_buf_cstr(&out), want);
        au_ws_parser_free(&p);
        au_buf_free(&out);
    }
}

#define V(s) (const uint8_t *)(s), sizeof(s) - 1

static void test_handshake_values(void)
{
    /* RFC 6455 section 1.3 */
    char acc[29];
    CHECK(au_ws_accept("dGhlIHNhbXBsZSBub25jZQ==", acc) && !strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="),
          "accept: %s", acc);
    uint8_t nonce[16];
    for (int i = 0; i < 16; i++)
        nonce[i] = (uint8_t)("the sample nonce"[i]);
    char key[25];
    au_ws_key(nonce, key);
    CHECK(!strcmp(key, "dGhlIHNhbXBsZSBub25jZQ=="), "key: %s", key);
}

static void test_frames(void)
{
    expect("text", V("\x81\x05Hello"), "T Hello;");
    expect("empty text", V("\x81\x00"), "T ;");
    expect("fragments", V("\x01\x03Hel\x80\x02lo"), "T Hello;");
    expect("ping inside fragments", V("\x01\x03Hel\x89\x02hi\x80\x02lo"), "P 2;T Hello;");
    expect("pong", V("\x8a\x00"), "O 0;");
    expect("binary 16-bit length", (const uint8_t *)"\x82\x7e\x00\x80"
                                                     "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
                                                     "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
           4 + 128, "B 128;");
    expect("close", V("\x88\x05\x03\xe8" "bye"), "C 1000 bye;");
    expect("close without body", V("\x88\x00"), "C 1005 ;");
    expect("nothing after close", V("\x88\x02\x03\xe8\x81\x01x"), "C 1000 ;");
    expect("two messages", V("\x81\x01" "a\x81\x01" "b"), "T a;T b;");
    expect("utf-8 text", V("\x81\x02\xc3\xa9"), "T \xc3\xa9;");
}

static void test_errors(void)
{
    expect("masked from server", V("\x81\x81\x00\x00\x00\x00x"), "E 1002;");
    expect("rsv bit", V("\xc1\x01x"), "E 1002;");
    expect("unknown opcode", V("\x83\x00"), "E 1002;");
    expect("fragmented ping", V("\x09\x00"), "E 1002;");
    expect("long ping", (const uint8_t *)"\x89\x7e\x00\x7e", 4, "E 1002;");
    expect("lone continuation", V("\x80\x01x"), "E 1002;");
    expect("new message inside one", V("\x01\x01x\x81\x01y"), "E 1002;");
    expect("bad utf-8", V("\x81\x02\xc3\x28"), "E 1007;");
    expect("truncated utf-8 across fragments", V("\x01\x01\xc3\x80\x00"), "E 1007;");
    expect("close 1 byte", V("\x88\x01\x03"), "E 1002;");
    expect("close code 1005", V("\x88\x02\x03\xed"), "E 1002;");
    expect("close code 999", V("\x88\x02\x03\xe7"), "E 1002;");
    expect("close reason bad utf-8", V("\x88\x03\x03\xe8\xff"), "E 1007;");
    expect("64-bit top bit", V("\x82\x7f\x80\x00\x00\x00\x00\x00\x00\x01"), "E 1002;");
    expect("too large", V("\x82\x7f\x00\x00\x00\x00\x00\x10\x00\x00"), "E 1009;");
    expect("nothing after an error", V("\xc1\x01x\x81\x01y"), "E 1002;");
}

static void test_build(void)
{
    const uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    AuBuf b;
    au_buf_init(&b);
    au_ws_frame(&b, AU_WS_TEXT, "Hello", 5, mask);
    /* RFC 6455 section 5.7: a masked "Hello" */
    static const uint8_t want[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
    CHECK(b.len == sizeof want && !memcmp(b.data, want, sizeof want), "masked Hello");
    au_buf_clear(&b);
    static uint8_t big[70000];
    au_ws_frame(&b, AU_WS_BINARY, big, sizeof big, mask);
    CHECK(b.len == sizeof big + 14 && (uint8_t)b.data[1] == (0x80 | 127), "64-bit length");
    au_buf_clear(&b);
    au_ws_frame(&b, AU_WS_BINARY, big, 300, mask);
    CHECK(b.len == 300 + 8 && (uint8_t)b.data[1] == (0x80 | 126) && (uint8_t)b.data[3] == 44, "16-bit length");
    au_buf_free(&b);
}

/* ---- Over the fake network ---- */

typedef struct Result {
    bool done;
    AuNetError err;
    int status, responses;
    AuBuf body;
    AuHttpTransfer *t;
    const char *reply; /* sent with au_http_tunnel_send on the first data */
    bool sent;
} Result;

static void r_response(void *ctx, AuHttpTransfer *t, const AuHttpResponse *r)
{
    Result *res = ctx;
    (void)t;
    res->status = r->status;
    res->responses++;
}
static void r_data(void *ctx, AuHttpTransfer *t, const void *d, size_t n)
{
    Result *res = ctx;
    au_buf_append(&res->body, d, n);
    if (res->reply && !res->sent)
        res->sent = au_http_tunnel_send(t, res->reply, strlen(res->reply));
}
static void r_done(void *ctx, AuHttpTransfer *t, AuNetError e)
{
    Result *res = ctx;
    (void)t;
    res->done = true;
    res->err = e;
}

static void open_ws(FakeNet *f, AuNet *net, Result *r, const char *url, int rounds, uint32_t step)
{
    memset(r, 0, sizeof *r);
    au_buf_init(&r->body);
    AuHttpRequest rq;
    au_http_request_init(&rq, url);
    rq.websocket = true;
    au_headers_add(&rq.headers, "Origin", "http://page.test");
    AuHttpCallbacks cb = {r, r_response, r_data, r_done, NULL, NULL};
    r->t = au_http_start(net, &rq, &cb);
    au_http_request_free(&rq);
    fake_run(f, net, rounds, step);
}

static void test_tunnel(void)
{
    FakeNet f;
    fake_init(&f);
    AuNet *net = au_net_new(&f.ops);
    FakeServer *s = fake_server(&f, "ws.test", 80);
    FakeResponse *fr = fake_add(s, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                   "Sec-WebSocket-Accept: {ACCEPT}\r\n\r\n\x81\x02hi");
    fr->websocket = true;
    Result r;
    open_ws(&f, net, &r, "http://ws.test/chat", 0, 0);
    r.reply = "\x81\x80\x01\x02\x03\x04";
    fake_run(&f, net, 200, 10);
    const char *q = s->nreq ? s->requests[0] : "";
    CHECK(strstr(q, "GET /chat HTTP/1.1\r\n") && strstr(q, "\r\nUpgrade: websocket\r\n") &&
              strstr(q, "\r\nConnection: Upgrade\r\n") && strstr(q, "\r\nSec-WebSocket-Version: 13\r\n") &&
              strstr(q, "\r\nSec-WebSocket-Key: ") && strstr(q, "\r\nOrigin: http://page.test\r\n") &&
              !strstr(q, "Accept-Encoding"),
          "handshake request: %s", q);
    CHECK(r.status == 101 && r.responses == 1, "101 delivered: %d", r.status);
    CHECK(r.body.len == 4 && !memcmp(r.body.data, "\x81\x02hi", 4), "frame after the head: %u bytes",
          (unsigned)r.body.len);
    CHECK(r.sent && s->tunnel.len == 6 && !memcmp(s->tunnel.data, "\x81\x80\x01\x02\x03\x04", 6),
          "sent bytes reach the server: %u", (unsigned)s->tunnel.len);
    CHECK(!r.done, "still open");
    au_http_cancel(r.t);
    fake_run(&f, net, 10, 10);
    au_buf_free(&r.body);
    au_net_free(net);
    CHECK(f.open_socks == 0, "sockets left open: %d", f.open_socks);
    fake_free(&f);
}

/* The server closes after its close frame: done with AU_NET_OK. */
static void test_close(void)
{
    FakeNet f;
    fake_init(&f);
    AuNet *net = au_net_new(&f.ops);
    FakeServer *s = fake_server(&f, "ws.test", 80);
    FakeResponse *fr = fake_add(s, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                   "Sec-WebSocket-Accept: {ACCEPT}\r\n\r\n\x88\x02\x03\xe8");
    fr->websocket = true;
    fr->close_after = true;
    Result r;
    open_ws(&f, net, &r, "http://ws.test/", 2000, 10);
    CHECK(r.status == 101 && r.body.len == 4, "close frame delivered: %u", (unsigned)r.body.len);
    CHECK(r.done && r.err == AU_NET_OK, "orderly close: done %d err %d", r.done, r.err);
    au_buf_free(&r.body);
    au_net_free(net);
    CHECK(f.open_socks == 0, "sockets left open: %d", f.open_socks);
    fake_free(&f);
}

static void test_handshake_refused(void)
{
    static const char *const bad[] = {
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nno",
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: h2c\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: {ACCEPT}\r\n\r\n",
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Accept: {ACCEPT}\r\n\r\n",
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: {ACCEPT}\r\nSec-WebSocket-Extensions: permessage-deflate\r\n\r\n",
        "HTTP/1.1 302 Found\r\nLocation: http://ws.test/other\r\nContent-Length: 0\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        FakeNet f;
        fake_init(&f);
        AuNet *net = au_net_new(&f.ops);
        FakeServer *s = fake_server(&f, "ws.test", 80);
        fake_add(s, bad[i])->websocket = true;
        Result r;
        open_ws(&f, net, &r, "http://ws.test/", 2000, 10);
        CHECK(r.done && r.err == AU_NET_ERR_PROTOCOL && r.responses == 0, "bad handshake %u: done %d err %d",
              (unsigned)i, r.done, r.err);
        CHECK(s->nreq == 1, "no redirect followed (%u)", (unsigned)i);
        au_buf_free(&r.body);
        au_net_free(net);
        fake_free(&f);
    }
}

static void test_no_idle_timeout(void)
{
    FakeNet f;
    fake_init(&f);
    AuNet *net = au_net_new(&f.ops);
    FakeServer *s = fake_server(&f, "ws.test", 80);
    fake_add(s, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                "Sec-WebSocket-Accept: {ACCEPT}\r\n\r\n")
        ->websocket = true;
    Result r;
    /* five minutes of silence */
    open_ws(&f, net, &r, "http://ws.test/", 3000, 100);
    CHECK(r.status == 101 && !r.done, "still open after a silent spell: done %d err %d", r.done, r.err);
    CHECK(au_http_tunnel_send(r.t, "x", 1), "send on an open tunnel");
    au_http_cancel(r.t);
    fake_run(&f, net, 10, 10);
    CHECK(!r.done, "no done after cancel");
    au_buf_free(&r.body);
    au_net_free(net);
    CHECK(f.open_socks == 0, "sockets left open: %d", f.open_socks);
    fake_free(&f);
}

int main(void)
{
    test_handshake_values();
    test_frames();
    test_errors();
    test_build();
    test_tunnel();
    test_close();
    test_handshake_refused();
    test_no_idle_timeout();
    printf("websocket: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
