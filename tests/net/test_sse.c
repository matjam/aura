/* The event stream parser (src/net/sse.c): vectors from the HTML
 * standard's "Parsing an event stream" and "Interpreting an event
 * stream", each fed whole, split in two at every point, and one byte at a
 * time; truncated streams; and the limits. */
#include "net/sse.h"

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

/* What the callbacks saw, as text: "E type|data|id;" for an event,
 * "I id;" for a blank line without one, "R ms;" for a retry. */
typedef struct Log {
    AuBuf b;
} Log;

static void on_dispatch(void *ctx, const char *type, size_t tlen, const char *data, size_t dlen, const char *id,
                        size_t idlen)
{
    Log *l = ctx;
    if (data) {
        au_buf_append(&l->b, "E ", 2);
        au_buf_append(&l->b, type, tlen);
        au_buf_putc(&l->b, '|');
        au_buf_append(&l->b, data, dlen);
        au_buf_putc(&l->b, '|');
    } else {
        au_buf_append(&l->b, "I ", 2);
    }
    au_buf_append(&l->b, id, idlen);
    au_buf_putc(&l->b, ';');
}

static void on_retry(void *ctx, uint32_t ms)
{
    Log *l = ctx;
    char t[24];
    int n = snprintf(t, sizeof t, "R %lu;", (unsigned long)ms);
    au_buf_append(&l->b, t, (size_t)n);
}

/* Feeds s (n bytes) in pieces: the cut points in cuts (ascending). */
static void run(const char *s, size_t n, const size_t *cuts, int ncuts, AuBuf *out)
{
    Log l;
    AuSseParser p;
    AuSseCallbacks cb = {&l, on_dispatch, on_retry};
    size_t at = 0;
    au_buf_init(&l.b);
    au_sse_init(&p);
    for (int i = 0; i <= ncuts; i++) {
        size_t end = i < ncuts ? cuts[i] : n;
        au_sse_feed(&p, s + at, end - at, &cb);
        at = end;
    }
    au_sse_free(&p);
    au_buf_clear(out);
    au_buf_append(out, l.b.data ? (const char *)l.b.data : "", l.b.len);
    au_buf_free(&l.b);
}

static void vec_n(const char *in, size_t n, const char *want, int line)
{
    AuBuf got, split;
    au_buf_init(&got);
    au_buf_init(&split);
    run(in, n, NULL, 0, &got);
    CHECK(!strcmp(au_buf_cstr(&got), want), "line %d: got \"%s\", want \"%s\"", line, au_buf_cstr(&got), want);
    for (size_t c = 0; c <= n; c++) {
        run(in, n, &c, 1, &split);
        if (strcmp(au_buf_cstr(&split), want)) {
            CHECK(0, "line %d: split at %zu: got \"%s\"", line, c, au_buf_cstr(&split));
            break;
        }
    }
    {
        size_t *cuts = malloc((n ? n : 1) * sizeof *cuts);
        for (size_t c = 0; c < n; c++)
            cuts[c] = c;
        run(in, n, cuts, (int)n, &split);
        CHECK(!strcmp(au_buf_cstr(&split), want), "line %d: byte at a time: got \"%s\"", line, au_buf_cstr(&split));
        free(cuts);
    }
    au_buf_free(&got);
    au_buf_free(&split);
}
#define VEC(in, want) vec_n(in, sizeof(in) - 1, want, __LINE__)

static void vectors(void)
{
    VEC("", "");
    VEC("data: one\n\n", "E message|one|;");
    VEC("data:one\n\n", "E message|one|;");
    VEC("data:  two spaces\n\n", "E message| two spaces|;");
    VEC("data: a\ndata: b\n\n", "E message|a\nb|;");
    VEC("data\n\n", "E message||;");
    VEC("data\ndata\n\n", "E message|\n|;");
    VEC("data:\n\n", "E message||;");
    /* line ends: CR LF, CR, LF, mixed */
    VEC("data: a\r\ndata: b\r\n\r\n", "E message|a\nb|;");
    VEC("data: a\rdata: b\r\r", "E message|a\nb|;");
    VEC("data: a\r\n\ndata: b\n\r\n", "E message|a|;E message|b|;");
    VEC("data: a\n\r\r\n", "E message|a|;I ;");
    /* event types */
    VEC("event: ping\ndata: x\n\n", "E ping|x|;");
    VEC("event: ping\n\ndata: x\n\n", "I ;E message|x|;");
    VEC("event:\ndata: x\n\n", "E message|x|;");
    VEC("event: a\nevent: b\ndata: x\n\n", "E b|x|;");
    /* ids */
    VEC("id: 7\ndata: a\n\ndata: b\n\n", "E message|a|7;E message|b|7;");
    VEC("id: 7\n\n", "I 7;");
    VEC("id: 7\n\nid\n\n", "I 7;I ;");
    VEC("id: 1\nid: 2\ndata: x\n\n", "E message|x|2;");
    VEC("id: 1\n\nid: a\0b\n\n", "I 1;I 1;");
    VEC("data: x\nid: 3\n\n", "E message|x|3;");
    /* retry */
    VEC("retry: 1000\n", "R 1000;");
    VEC("retry:0\n", "R 0;");
    VEC("retry: 1a\nretry: -5\nretry:  10\nretry:\nretry: 1.5\n", "");
    VEC("retry: 99999999999999999999\n", "R 2147483647;");
    VEC("retry: 2147483648\n", "R 2147483647;");
    /* comments and unknown fields */
    VEC(":comment\ndata: x\n\n", "E message|x|;");
    VEC(": a comment\n\n", "I ;");
    VEC("foo: bar\nData: no\nDATA: no\n\n", "I ;");
    VEC("data : no\n\n", "I ;");
    VEC(" data: no\n\n", "I ;");
    /* a partial event at the end is dropped */
    VEC("data: a\n", "");
    VEC("data: a\n\ndata: b", "E message|a|;");
    VEC("data: a", "");
    /* the byte order mark: once, at the start */
    VEC("\xEF\xBB\xBF" "data: b\n\n", "E message|b|;");
    VEC("\xEF\xBB\xBF\xEF\xBB\xBF" "data: b\n\n", "I ;");
    VEC("\xEF\xBB" "data: b\n\n", "I ;");
    VEC("\xEF\xEF\xBB\xBF" "data: b\n\n", "I ;");
    VEC("\xEF\xBB\xBF", "");
    /* text that is not UTF-8 becomes U+FFFD */
    VEC("data: \xFF\n\n", "E message|\xEF\xBF\xBD|;");
    VEC("data: \xC3\n\n", "E message|\xEF\xBF\xBD|;");
    VEC("data: caf\xC3\xA9\n\n", "E message|caf\xC3\xA9|;");
    VEC("data: \xF0\x9F\x98\x80\n\n", "E message|\xF0\x9F\x98\x80|;");
    VEC("data: \xED\xA0\x80\n\n", "E message|\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD|;");
    VEC("event: \xFE\ndata: x\nid: \x80\n\n", "E \xEF\xBF\xBD|x|\xEF\xBF\xBD;");
    VEC("data: a\0b\n\n", "E message|a\0b|;");
}

/* The limits: an over-long line is dropped to its end, an over-large
 * event is dropped whole, and the parser carries on after both. */
static void limits(void)
{
    Log l;
    AuSseParser p;
    AuSseCallbacks cb = {&l, on_dispatch, on_retry};
    au_buf_init(&l.b);
    au_sse_init(&p);
    p.max_line = 32;
    p.max_data = 64;
    char big[100];
    memset(big, 'x', sizeof big);
    au_sse_feed(&p, "data: ", 6, &cb);
    au_sse_feed(&p, big, sizeof big, &cb);
    au_sse_feed(&p, "\ndata: ok\n\n", 11, &cb);
    CHECK(!strcmp(au_buf_cstr(&l.b), "E message|ok|;"), "long line: %s", au_buf_cstr(&l.b));
    au_buf_clear(&l.b);
    for (int i = 0; i < 6; i++)
        au_sse_feed(&p, "data: 0123456789abcdef\n", 23, &cb);
    au_sse_feed(&p, "\n", 1, &cb);
    CHECK(!strcmp(au_buf_cstr(&l.b), "I ;"), "large event: %s", au_buf_cstr(&l.b));
    au_buf_clear(&l.b);
    au_sse_feed(&p, "data: after\n\n", 13, &cb);
    CHECK(!strcmp(au_buf_cstr(&l.b), "E message|after|;"), "after the large event: %s", au_buf_cstr(&l.b));
    CHECK(p.events == 2, "events %lu", (unsigned long)p.events);
    au_sse_free(&p);
    au_buf_free(&l.b);
}

/* au_sse_set_id: a reconnection starts with the last event ID. */
static void set_id(void)
{
    Log l;
    AuSseParser p;
    AuSseCallbacks cb = {&l, on_dispatch, NULL};
    au_buf_init(&l.b);
    au_sse_init(&p);
    au_sse_set_id(&p, "42", 2);
    au_sse_feed(&p, "data: x\n\nretry: 5\n", 18, &cb);
    CHECK(!strcmp(au_buf_cstr(&l.b), "E message|x|42;"), "set id: %s", au_buf_cstr(&l.b));
    au_sse_free(&p);
    au_buf_free(&l.b);
}

/* A long stream of every byte value, cut short at each length: no
 * callback sees invalid UTF-8 and nothing reads past the end (the
 * sanitizer build checks the second). */
static int valid_utf8(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        size_t k;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF)
            k = 1, cp = c & 0x1F;
        else if (c >= 0xE0 && c <= 0xEF)
            k = 2, cp = c & 0x0F;
        else if (c >= 0xF0 && c <= 0xF4)
            k = 3, cp = c & 0x07;
        else
            return 0;
        for (size_t j = 1; j <= k; j++) {
            if (i + j >= n || (s[i + j] & 0xC0) != 0x80)
                return 0;
            cp = cp << 6 | (s[i + j] & 0x3F);
        }
        if ((k == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (k == 3 && (cp < 0x10000 || cp > 0x10FFFF)))
            return 0;
        i += k + 1;
    }
    return 1;
}

typedef struct Check {
    int bad;
} Check;

static void check_dispatch(void *ctx, const char *type, size_t tlen, const char *data, size_t dlen, const char *id,
                           size_t idlen)
{
    Check *c = ctx;
    if ((type && !valid_utf8((const unsigned char *)type, tlen)) ||
        (data && !valid_utf8((const unsigned char *)data, dlen)) || !valid_utf8((const unsigned char *)id, idlen) ||
        id[idlen] != 0 || (data && data[dlen] != 0))
        c->bad++;
}

static void truncation(void)
{
    static const char base[] = "\xEF\xBB\xBFretry: 10\r\nid: 1\nevent: e\ndata: caf\xC3\xA9\r\rdata: \xF0\x9F\x98\x80\n"
                               ":c\n\ndata: \xFF\xC3\nid: x\0y\nid\n\n";
    char all[sizeof base + 256];
    size_t n = sizeof base - 1;
    memcpy(all, base, n);
    for (int b = 0; b < 256; b++)
        all[n + (size_t)b] = (char)b;
    n += 256;
    for (size_t len = 0; len <= n; len++) {
        Check c = {0};
        AuSseParser p;
        AuSseCallbacks cb = {&c, check_dispatch, NULL};
        char *copy = malloc(len ? len : 1);
        memcpy(copy, all, len);
        au_sse_init(&p);
        au_sse_feed(&p, copy, len, &cb);
        au_sse_free(&p);
        free(copy);
        CHECK(!c.bad, "truncated at %zu: invalid UTF-8 or no NUL in a callback", len);
    }
}

int main(void)
{
    vectors();
    limits();
    set_id();
    truncation();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
