/* The event stream parser of EventSource (HTML, "Parsing an event
 * stream"): text/event-stream bytes in, events out. Sans I/O: feed it
 * the body as it arrives, in pieces split anywhere.
 *
 *     AuSseParser p;
 *     au_sse_init(&p);
 *     au_sse_feed(&p, bytes, len, &callbacks);   // as often as data comes
 *     au_sse_free(&p);                           // the end; a partial event is dropped
 *
 * What it does, as the standard says:
 * - a UTF-8 byte order mark at the start of the stream is skipped, and
 *   every other byte sequence that is not UTF-8 becomes U+FFFD, so what
 *   the callbacks get is always valid UTF-8;
 * - lines end at CR LF, LF or CR (a CR at the end of one piece and an LF
 *   at the start of the next are one line end);
 * - a line that starts with a colon is a comment;
 * - "event" sets the type, "data" appends to the data (with LF between
 *   lines), "id" sets the last event ID unless the value holds NUL,
 *   "retry" with only ASCII digits sets the reconnection time; other
 *   fields are ignored;
 * - a blank line dispatches: the last event ID is reported, and an event
 *   if there is data (its trailing LF removed), with the type "message"
 *   when none was given.
 *
 * Limits (the data is a hostile server's): a line longer than max_line
 * bytes is dropped to its end, and an event whose data grows past
 * max_data bytes is dropped whole (au_sse_init sets 512 KB and 1 MB).
 * A retry value too large for 31 bits is held at 0x7fffffff ms. */
#ifndef AURA_NET_SSE_H
#define AURA_NET_SSE_H

#include "../core/core.h"

typedef struct AuSseCallbacks {
    void *ctx;
    /* A blank line. id is the last event ID (it stays set until an "id"
     * field changes it). With an event, type and data are set; data NULL
     * means the blank line dispatched no event (no data), and the caller
     * only updates its last event ID. The strings are valid UTF-8,
     * NUL-terminated, and valid during the call. */
    void (*dispatch)(void *ctx, const char *type, size_t type_len, const char *data, size_t data_len,
                     const char *id, size_t id_len);
    /* A valid "retry" field, in milliseconds. Optional. */
    void (*retry)(void *ctx, uint32_t ms);
} AuSseCallbacks;

typedef struct AuSseParser {
    AuBuf line;      /* the line so far, as received */
    AuBuf data;      /* the data buffer, UTF-8 */
    AuBuf type;      /* the event type buffer, UTF-8 */
    AuBuf id;        /* the last event ID buffer, UTF-8 */
    uint8_t bom;     /* BOM bytes seen at the start (0 to 3) */
    bool started;    /* past the BOM check */
    bool cr;         /* the last byte was CR: an LF next ends nothing */
    bool skip_line;  /* the line is past max_line: drop it */
    bool data_over;  /* the data is past max_data: drop the event */
    size_t max_line, max_data;
    uint64_t events; /* events dispatched (for tests) */
} AuSseParser;

void au_sse_init(AuSseParser *p);
void au_sse_free(AuSseParser *p);
/* Parses len more bytes; the callbacks run for each line end that
 * completes a dispatch. */
void au_sse_feed(AuSseParser *p, const void *bytes, size_t len, const AuSseCallbacks *cb);
/* Sets the last event ID buffer (EventSource restores it after a
 * reconnection from its own copy; the parser is new each time). */
void au_sse_set_id(AuSseParser *p, const char *id, size_t len);

#endif
