/* HTTP/1.1 response parser (RFC 9112), sans I/O: it consumes bytes and
 * reports events, and never touches a socket.
 *
 *     AuHttpParser p;
 *     au_http_parser_init(&p, is_head_request);
 *     for each chunk of received bytes:
 *         while (len) {
 *             size_t used;
 *             AuHttpEvent ev;
 *             int r = au_http_parser_feed(&p, data, len, &used, &ev);
 *             data += used; len -= used;
 *             ...handle r...
 *         }
 *     on connection close: au_http_parser_eof(&p)
 *
 * Interim (1xx) responses other than 101 are skipped. Framing: no body
 * for HEAD, 1xx, 204 and 304; chunked when Transfer-Encoding ends in
 * chunked (it then wins over Content-Length, and the connection is not
 * reused); Content-Length (identical repeated values are accepted,
 * different ones are an error); otherwise the body runs to the close.
 *
 * Limits: the status line and header block together at most
 * AU_HTTP_MAX_HEADER_BYTES, at most AU_HTTP_MAX_HEADERS fields, chunk
 * size lines at most 1 KB, trailers at most 16 KB. Obsolete line folding,
 * whitespace before a field's colon, NUL or bare CR in a field, and a
 * malformed chunk are errors. */
#ifndef AURA_NET_HTTP_PARSE_H
#define AURA_NET_HTTP_PARSE_H

#include "headers.h"

#define AU_HTTP_MAX_HEADER_BYTES (96u * 1024)
#define AU_HTTP_MAX_HEADERS 160
#define AU_HTTP_MAX_INTERIM 16

typedef enum {
    AU_HP_NEED_MORE, /* all input consumed; feed more */
    AU_HP_HEADERS,   /* status and headers are ready (p->status, p->headers) */
    AU_HP_BODY,      /* ev->data/ev->len: body bytes (pointing into the input) */
    AU_HP_DONE,      /* the message is complete; bytes after it belong to the next */
    AU_HP_ERROR      /* malformed; p->error says why */
} AuHttpParseResult;

typedef struct AuHttpEvent {
    const uint8_t *data;
    size_t len;
} AuHttpEvent;

typedef enum { AU_BODY_NONE, AU_BODY_LENGTH, AU_BODY_CHUNKED, AU_BODY_CLOSE } AuBodyFraming;

typedef struct AuHttpParser {
    int state;
    bool head;
    int status;
    int version_minor; /* HTTP/1.x */
    char reason[64];
    AuHeaders headers;
    AuHeaders trailers;
    AuBodyFraming framing;
    int64_t remaining;    /* content length or current chunk bytes left */
    int64_t content_length; /* -1 when not given */
    bool keep_alive;      /* the connection may be reused after this message */
    int interim;
    AuBuf line;           /* header block or chunk line being collected */
    size_t trailer_bytes;
    const char *error;
} AuHttpParser;

void au_http_parser_init(AuHttpParser *p, bool head_request);
void au_http_parser_free(AuHttpParser *p);
AuHttpParseResult au_http_parser_feed(AuHttpParser *p, const uint8_t *data, size_t len, size_t *used,
                                      AuHttpEvent *ev);
/* The connection closed. Returns AU_HP_DONE when that ends the message
 * properly (close-delimited body), AU_HP_ERROR otherwise (truncated). */
AuHttpParseResult au_http_parser_eof(AuHttpParser *p);
/* True once headers have been reported. */
bool au_http_parser_have_headers(const AuHttpParser *p);

#endif
