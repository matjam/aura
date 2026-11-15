/* HTTP/1.1 response parser. See http_parse.h. */
#include "http_parse.h"

#include <string.h>

enum { P_HEAD, P_BODY_LENGTH, P_BODY_CLOSE, P_CHUNK_SIZE, P_CHUNK_DATA, P_CHUNK_CRLF, P_TRAILERS, P_DONE, P_ERROR };

void au_http_parser_init(AuHttpParser *p, bool head_request)
{
    memset(p, 0, sizeof *p);
    p->head = head_request;
    p->state = P_HEAD;
    p->content_length = -1;
    au_headers_init(&p->headers);
    au_headers_init(&p->trailers);
    au_buf_init(&p->line);
}

void au_http_parser_free(AuHttpParser *p)
{
    au_headers_free(&p->headers);
    au_headers_free(&p->trailers);
    au_buf_free(&p->line);
}

bool au_http_parser_have_headers(const AuHttpParser *p)
{
    return p->state != P_HEAD && p->state != P_ERROR;
}

static AuHttpParseResult fail(AuHttpParser *p, const char *why)
{
    p->state = P_ERROR;
    p->error = why;
    return AU_HP_ERROR;
}

/* Parses "HTTP/1.x SSS reason". */
static bool parse_status(AuHttpParser *p, const char *s, size_t n)
{
    if (n < 12 || memcmp(s, "HTTP/1.", 7) != 0 || !au_is_digit((unsigned char)s[7]) || s[8] != ' ')
        return false;
    p->version_minor = s[7] - '0';
    if (!au_is_digit((unsigned char)s[9]) || !au_is_digit((unsigned char)s[10]) || !au_is_digit((unsigned char)s[11]))
        return false;
    p->status = (s[9] - '0') * 100 + (s[10] - '0') * 10 + (s[11] - '0');
    if (p->status < 100)
        return false;
    if (n > 12 && s[12] != ' ')
        return false;
    size_t rl = n > 13 ? n - 13 : 0;
    if (rl >= sizeof p->reason)
        rl = sizeof p->reason - 1;
    for (size_t i = 0; i < rl; i++) {
        unsigned char c = (unsigned char)s[13 + i];
        p->reason[i] = (char)(c < 0x20 || c == 0x7f ? '?' : c);
    }
    p->reason[rl] = 0;
    return true;
}

/* Parses Content-Length values: "42" or "42, 42". */
static bool parse_length(const AuHeaders *h, int64_t *out)
{
    int64_t v = -1;
    size_t pos = 0;
    const char *s;
    while ((s = au_headers_next(h, "content-length", &pos)) != NULL) {
        const char *item;
        size_t il;
        bool any = false;
        while (au_http_list_next(&s, &item, &il)) {
            any = true;
            int64_t x = 0;
            if (il == 0 || il > 15)
                return false;
            for (size_t i = 0; i < il; i++) {
                if (!au_is_digit((unsigned char)item[i]))
                    return false;
                x = x * 10 + (item[i] - '0');
            }
            if (v >= 0 && x != v)
                return false;
            v = x;
        }
        if (!any)
            return false;
    }
    *out = v;
    return true;
}

/* Interprets the collected header block. Returns 1 for a final response,
 * 0 for a skipped interim response, -1 on error. */
static int finish_head(AuHttpParser *p)
{
    char *s = p->line.data;
    size_t n = p->line.len;
    au_headers_clear(&p->headers);
    /* Lines end with LF (CR before it is dropped). */
    size_t i = 0;
    bool first = true;
    while (i < n) {
        size_t j = i;
        while (j < n && s[j] != '\n')
            j++;
        size_t e = j;
        if (e > i && s[e - 1] == '\r')
            e--;
        if (e == i) /* the blank line */
            break;
        for (size_t k = i; k < e; k++)
            if (s[k] == '\r' || s[k] == 0) {
                p->error = "CR or NUL inside a header line";
                return -1;
            }
        if (first) {
            if (!parse_status(p, s + i, e - i)) {
                p->error = "bad status line";
                return -1;
            }
            first = false;
        } else {
            if (s[i] == ' ' || s[i] == '\t') {
                p->error = "obsolete line folding";
                return -1;
            }
            size_t c = i;
            while (c < e && s[c] != ':')
                c++;
            if (c == e || c == i) {
                p->error = "header line without a name and colon";
                return -1;
            }
            for (size_t k = i; k < c; k++)
                if (!au_http_is_tchar((unsigned char)s[k])) {
                    p->error = "invalid header name";
                    return -1;
                }
            size_t vs = c + 1, ve = e;
            while (vs < ve && (s[vs] == ' ' || s[vs] == '\t'))
                vs++;
            while (ve > vs && (s[ve - 1] == ' ' || s[ve - 1] == '\t'))
                ve--;
            if (p->headers.n >= AU_HTTP_MAX_HEADERS) {
                p->error = "too many header fields";
                return -1;
            }
            au_headers_addn(&p->headers, s + i, c - i, s + vs, ve - vs);
        }
        i = j + 1;
    }
    if (first) {
        p->error = "empty response head";
        return -1;
    }
    au_buf_clear(&p->line);
    if (p->status >= 100 && p->status < 200) {
        if (p->status == 101) {
            p->error = "unexpected protocol switch";
            return -1;
        }
        if (++p->interim > AU_HTTP_MAX_INTERIM) {
            p->error = "too many interim responses";
            return -1;
        }
        return 0;
    }
    /* Framing. */
    p->keep_alive = p->version_minor >= 1 ? !au_headers_has_token(&p->headers, "connection", "close")
                                          : au_headers_has_token(&p->headers, "connection", "keep-alive");
    int64_t cl;
    if (!parse_length(&p->headers, &cl)) {
        p->error = "invalid Content-Length";
        return -1;
    }
    p->content_length = cl;
    const char *te = au_headers_get(&p->headers, "transfer-encoding");
    if (p->head || p->status == 204 || p->status == 304) {
        p->framing = AU_BODY_NONE;
    } else if (te) {
        /* The last coding decides; anything other than a lone chunked is
         * not supported (gzip in Transfer-Encoding is obsolete). */
        AuBuf all;
        au_buf_init(&all);
        au_headers_get_combined(&p->headers, "transfer-encoding", &all);
        const char *l = au_buf_cstr(&all), *item = NULL, *last = NULL;
        size_t il = 0, lastl = 0, count = 0;
        while (au_http_list_next(&l, &item, &il)) {
            last = item;
            lastl = il;
            count++;
        }
        bool chunked = last && au_ieq(last, lastl, "chunked", 7);
        au_buf_free(&all);
        if (chunked && count == 1) {
            p->framing = AU_BODY_CHUNKED;
        } else if (!chunked) {
            p->framing = AU_BODY_CLOSE;
            p->keep_alive = false;
        } else {
            p->error = "unsupported transfer coding";
            return -1;
        }
        if (cl >= 0)
            p->keep_alive = false; /* both: possible smuggling; do not reuse */
    } else if (cl >= 0) {
        p->framing = AU_BODY_LENGTH;
        p->remaining = cl;
    } else {
        p->framing = AU_BODY_CLOSE;
        p->keep_alive = false;
    }
    return 1;
}

static void enter_body(AuHttpParser *p)
{
    switch (p->framing) {
    case AU_BODY_NONE: p->state = P_DONE; break;
    case AU_BODY_LENGTH: p->state = p->remaining ? P_BODY_LENGTH : P_DONE; break;
    case AU_BODY_CHUNKED: p->state = P_CHUNK_SIZE; break;
    default: p->state = P_BODY_CLOSE; break;
    }
}

/* Collects one line (up to LF) into p->line. Returns true when complete;
 * *i advances. */
static bool collect_line(AuHttpParser *p, const uint8_t *d, size_t n, size_t *i, size_t max, bool *too_long)
{
    *too_long = false;
    const uint8_t *nl = memchr(d + *i, '\n', n - *i);
    size_t take = nl ? (size_t)(nl - (d + *i)) + 1 : n - *i;
    if (p->line.len + take > max) {
        *too_long = true;
        return false;
    }
    au_buf_append(&p->line, d + *i, take);
    *i += take;
    return nl != NULL;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

AuHttpParseResult au_http_parser_feed(AuHttpParser *p, const uint8_t *d, size_t n, size_t *used, AuHttpEvent *ev)
{
    size_t i = 0;
    ev->data = NULL;
    ev->len = 0;
    AuHttpParseResult r = AU_HP_NEED_MORE;
    while (i < n || p->state == P_DONE) {
        switch (p->state) {
        case P_HEAD: {
            /* Skip blank lines before a status line. */
            while (p->line.len == 0 && i < n && (d[i] == '\r' || d[i] == '\n'))
                i++;
            if (i >= n)
                goto out;
            bool done = false;
            while (i < n) {
                uint8_t c = d[i++];
                if (p->line.len >= AU_HTTP_MAX_HEADER_BYTES) {
                    r = fail(p, "response head too large");
                    goto out;
                }
                au_buf_putc(&p->line, (char)c);
                if (c == '\n') {
                    size_t L = p->line.len;
                    const char *b = p->line.data;
                    if ((L >= 2 && b[L - 2] == '\n') || (L >= 3 && b[L - 2] == '\r' && b[L - 3] == '\n')) {
                        done = true;
                        break;
                    }
                    /* An HTTP/0.9 or garbage response has no status line:
                     * catch it at the first line. */
                    if (memchr(b, '\n', L - 1) == NULL && (L < 8 || memcmp(b, "HTTP/1.", 7) != 0)) {
                        r = fail(p, "bad status line");
                        goto out;
                    }
                }
            }
            if (!done) {
                if (p->line.len >= 7 && memcmp(p->line.data, "HTTP/1.", 7) != 0) {
                    r = fail(p, "bad status line");
                    goto out;
                }
                goto out;
            }
            int h = finish_head(p);
            if (h < 0) {
                r = fail(p, p->error);
                goto out;
            }
            if (h == 0)
                continue; /* interim: parse the next head */
            enter_body(p);
            r = AU_HP_HEADERS;
            goto out;
        }
        case P_BODY_LENGTH: {
            size_t take = n - i;
            if ((int64_t)take > p->remaining)
                take = (size_t)p->remaining;
            ev->data = d + i;
            ev->len = take;
            i += take;
            p->remaining -= (int64_t)take;
            if (p->remaining == 0)
                p->state = P_DONE;
            r = AU_HP_BODY;
            goto out;
        }
        case P_BODY_CLOSE:
            ev->data = d + i;
            ev->len = n - i;
            i = n;
            r = AU_HP_BODY;
            goto out;
        case P_CHUNK_SIZE: {
            bool too_long;
            if (!collect_line(p, d, n, &i, 1024, &too_long)) {
                if (too_long) {
                    r = fail(p, "chunk size line too long");
                    goto out;
                }
                goto out;
            }
            const char *s = p->line.data;
            size_t L = p->line.len, k = 0;
            int64_t size = 0;
            int digits = 0;
            while (k < L && hexv((unsigned char)s[k]) >= 0) {
                if (++digits > 15) {
                    r = fail(p, "chunk size overflow");
                    goto out;
                }
                size = size * 16 + hexv((unsigned char)s[k]);
                k++;
            }
            if (!digits) {
                r = fail(p, "bad chunk size");
                goto out;
            }
            while (k < L && (s[k] == ' ' || s[k] == '\t'))
                k++;
            /* After the size: an extension (";..."), then CRLF or LF. */
            if (k < L && s[k] == ';') {
                for (; k < L && s[k] != '\r' && s[k] != '\n'; k++)
                    if (s[k] == 0) {
                        r = fail(p, "NUL in chunk extension");
                        goto out;
                    }
            }
            if (!(k < L && (s[k] == '\n' || (s[k] == '\r' && k + 1 < L && s[k + 1] == '\n')))) {
                r = fail(p, "bad chunk size line");
                goto out;
            }
            au_buf_clear(&p->line);
            if (size == 0) {
                p->state = P_TRAILERS;
                p->trailer_bytes = 0;
            } else {
                p->remaining = size;
                p->state = P_CHUNK_DATA;
            }
            break;
        }
        case P_CHUNK_DATA: {
            size_t take = n - i;
            if ((int64_t)take > p->remaining)
                take = (size_t)p->remaining;
            ev->data = d + i;
            ev->len = take;
            i += take;
            p->remaining -= (int64_t)take;
            if (p->remaining == 0)
                p->state = P_CHUNK_CRLF;
            r = AU_HP_BODY;
            goto out;
        }
        case P_CHUNK_CRLF: {
            /* Exactly CRLF (or LF) after the data. */
            uint8_t c = d[i++];
            if (p->line.len == 0 && c == '\r') {
                au_buf_putc(&p->line, '\r');
            } else if (c == '\n') {
                au_buf_clear(&p->line);
                p->state = P_CHUNK_SIZE;
            } else {
                r = fail(p, "missing CRLF after chunk data");
                goto out;
            }
            break;
        }
        case P_TRAILERS: {
            bool too_long;
            size_t before = p->line.len;
            if (!collect_line(p, d, n, &i, 16384 - (p->trailer_bytes < 16384 ? p->trailer_bytes : 16384),
                              &too_long)) {
                if (too_long) {
                    r = fail(p, "trailers too large");
                    goto out;
                }
                goto out;
            }
            (void)before;
            const char *s = p->line.data;
            size_t L = p->line.len;
            p->trailer_bytes += L;
            if (L == 1 || (L == 2 && s[0] == '\r')) {
                au_buf_clear(&p->line);
                p->state = P_DONE;
                break;
            }
            /* A trailer field: keep it (with the same checks as headers). */
            size_t e = L - 1;
            if (e > 0 && s[e - 1] == '\r')
                e--;
            const char *colon = memchr(s, ':', e);
            if (!colon || colon == s || s[0] == ' ' || s[0] == '\t' || memchr(s, 0, e)) {
                r = fail(p, "bad trailer field");
                goto out;
            }
            for (const char *q = s; q < colon; q++)
                if (!au_http_is_tchar((unsigned char)*q)) {
                    r = fail(p, "bad trailer field");
                    goto out;
                }
            if (p->trailers.n < AU_HTTP_MAX_HEADERS) {
                const char *v = colon + 1, *ve = s + e;
                while (v < ve && (*v == ' ' || *v == '\t'))
                    v++;
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
                    ve--;
                au_headers_addn(&p->trailers, s, (size_t)(colon - s), v, (size_t)(ve - v));
            }
            au_buf_clear(&p->line);
            break;
        }
        case P_DONE:
            r = AU_HP_DONE;
            goto out;
        case P_ERROR:
        default:
            r = AU_HP_ERROR;
            goto out;
        }
    }
out:
    *used = i;
    if (r == AU_HP_NEED_MORE && p->state == P_ERROR)
        r = AU_HP_ERROR;
    return r;
}

AuHttpParseResult au_http_parser_eof(AuHttpParser *p)
{
    if (p->state == P_BODY_CLOSE || p->state == P_DONE) {
        p->state = P_DONE;
        return AU_HP_DONE;
    }
    if (p->state != P_ERROR)
        fail(p, p->state == P_HEAD ? "connection closed before the response head" : "response body truncated");
    return AU_HP_ERROR;
}
