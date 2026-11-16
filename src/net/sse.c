/* The event stream parser of EventSource (see sse.h). */
#include "sse.h"

#include <string.h>

void au_sse_init(AuSseParser *p)
{
    memset(p, 0, sizeof *p);
    au_buf_init(&p->line);
    au_buf_init(&p->data);
    au_buf_init(&p->type);
    au_buf_init(&p->id);
    p->max_line = 512u * 1024;
    p->max_data = 1024u * 1024;
}

void au_sse_free(AuSseParser *p)
{
    au_buf_free(&p->line);
    au_buf_free(&p->data);
    au_buf_free(&p->type);
    au_buf_free(&p->id);
}

/* Appends s (n bytes as received) to b as valid UTF-8: every malformed
 * sequence becomes U+FFFD. */
static void put_utf8(AuBuf *b, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t run = i;
        while (run < n && (unsigned char)s[run] < 0x80)
            run++;
        if (run > i) {
            au_buf_append(b, s + i, run - i);
            i = run;
            continue;
        }
        size_t adv;
        uint32_t cp = au_utf8_decode(s + i, n - i, &adv);
        au_buf_put_utf8(b, cp);
        i += adv;
    }
}

void au_sse_set_id(AuSseParser *p, const char *id, size_t len)
{
    au_buf_clear(&p->id);
    if (id && len)
        put_utf8(&p->id, id, len);
}

static void dispatch(AuSseParser *p, const AuSseCallbacks *cb)
{
    /* The last event ID is the caller's from now on, event or not. */
    const char *id = au_buf_cstr(&p->id);
    if (p->data_over || p->data.len == 0) {
        if (cb->dispatch)
            cb->dispatch(cb->ctx, NULL, 0, NULL, 0, id, p->id.len);
    } else {
        size_t n = p->data.len;
        if (p->data.data[n - 1] == '\n')
            n--;
        p->data.data[n] = 0;
        const char *type = p->type.len ? au_buf_cstr(&p->type) : "message";
        size_t tlen = p->type.len ? p->type.len : 7;
        p->events++;
        if (cb->dispatch)
            cb->dispatch(cb->ctx, type, tlen, p->data.data, n, id, p->id.len);
    }
    au_buf_clear(&p->data);
    au_buf_clear(&p->type);
    p->data_over = false;
}

static bool is_field(const char *f, size_t n, const char *name)
{
    size_t k = strlen(name);
    return n == k && !memcmp(f, name, k);
}

/* One complete line (without its end). */
static void process_line(AuSseParser *p, const char *s, size_t n, const AuSseCallbacks *cb)
{
    if (n == 0) {
        dispatch(p, cb);
        return;
    }
    if (s[0] == ':')
        return; /* a comment */
    const char *colon = memchr(s, ':', n);
    size_t flen = colon ? (size_t)(colon - s) : n;
    const char *v = colon ? colon + 1 : s + n;
    size_t vlen = colon ? n - flen - 1 : 0;
    if (vlen && v[0] == ' ') {
        v++;
        vlen--;
    }
    if (is_field(s, flen, "event")) {
        au_buf_clear(&p->type);
        put_utf8(&p->type, v, vlen);
    } else if (is_field(s, flen, "data")) {
        if (p->data_over)
            return;
        if (p->data.len + vlen + 1 > p->max_data) {
            /* Too large: the event is dropped at its blank line. */
            p->data_over = true;
            au_buf_free(&p->data);
            au_buf_init(&p->data);
            return;
        }
        put_utf8(&p->data, v, vlen);
        au_buf_putc(&p->data, '\n');
    } else if (is_field(s, flen, "id")) {
        if (!memchr(v, 0, vlen)) {
            au_buf_clear(&p->id);
            put_utf8(&p->id, v, vlen);
        }
    } else if (is_field(s, flen, "retry")) {
        if (!vlen)
            return;
        uint32_t ms = 0;
        for (size_t i = 0; i < vlen; i++) {
            if (v[i] < '0' || v[i] > '9')
                return;
            uint32_t d = (uint32_t)(v[i] - '0');
            ms = ms > (0x7fffffffu - d) / 10 ? 0x7fffffffu : ms * 10 + d;
        }
        if (cb->retry)
            cb->retry(cb->ctx, ms);
    }
}

/* The line held in p->line is complete. */
static void end_line(AuSseParser *p, const AuSseCallbacks *cb)
{
    if (!p->skip_line)
        process_line(p, p->line.data ? p->line.data : "", p->line.len, cb);
    p->skip_line = false;
    au_buf_clear(&p->line);
}

/* Adds bytes of the current line, minding the limit. */
static void add_to_line(AuSseParser *p, const char *s, size_t n)
{
    if (p->skip_line || !n)
        return;
    if (p->line.len + n > p->max_line) {
        p->skip_line = true;
        au_buf_free(&p->line);
        au_buf_init(&p->line);
        return;
    }
    au_buf_append(&p->line, s, n);
}

void au_sse_feed(AuSseParser *p, const void *bytes, size_t len, const AuSseCallbacks *cb)
{
    const char *s = bytes;
    size_t i = 0;
    /* The byte order mark, once, at the start of the stream (it may come
     * split over pieces). */
    while (!p->started && i < len) {
        static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        if ((unsigned char)s[i] == bom[p->bom]) {
            p->bom++;
            i++;
            if (p->bom == 3)
                p->started = true;
            continue;
        }
        /* Not a BOM after all: the bytes matched so far are text. */
        p->started = true;
        if (p->bom)
            add_to_line(p, (const char *)bom, p->bom);
        p->bom = 0;
    }
    while (i < len) {
        if (p->cr) {
            p->cr = false;
            if (s[i] == '\n') {
                i++;
                continue;
            }
        }
        size_t j = i;
        while (j < len && s[j] != '\r' && s[j] != '\n')
            j++;
        add_to_line(p, s + i, j - i);
        if (j == len)
            break;
        p->cr = s[j] == '\r';
        i = j + 1;
        end_line(p, cb);
    }
}
