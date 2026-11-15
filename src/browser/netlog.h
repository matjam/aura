/* The request log of a page, for the Network window: one record per
 * request since the page's last navigation, in the order the requests
 * started. Portable C99.
 *
 * The page (page_doc.c) owns one log and fills it as its fetches start,
 * get a response, receive data and end. The log is a ring of
 * AU_NETLOG_MAX records: when it is full the oldest record goes, and the
 * totals still count it. A record costs its URL and about 300 bytes;
 * header text is kept only when the fetcher supplies it (the network
 * stack records headers after au_net_set_trace_headers), at most
 * AU_NETLOG_HEAD_MAX bytes each way. Everything is freed with the page.
 *
 * Times are milliseconds. A record's start is measured from the
 * navigation; its phase times from its own start. */
#ifndef AURA_BROWSER_NETLOG_H
#define AURA_BROWSER_NETLOG_H

#include "page.h"

#define AU_NETLOG_MAX 300
#define AU_NETLOG_HEAD_MAX 4096

typedef enum {
    AU_NETREC_PENDING,
    AU_NETREC_DONE,
    AU_NETREC_FAILED,
    AU_NETREC_BLOCKED,  /* refused by policy: blocked_reason says which */
    AU_NETREC_CANCELLED /* the page stopped it (a new navigation, Stop) */
} AuNetRecState;

/* Where the bytes came from, beyond AuFetchSource's values. */
enum { AU_NETREC_FROM_FILE = 16, AU_NETREC_FROM_DATA, AU_NETREC_FROM_UNKNOWN };

typedef struct AuNetRecord {
    uint32_t seq;      /* 1 for the first request of the navigation */
    char *url;
    char method[8];
    uint8_t kind;      /* AuFetchKind */
    uint8_t state;     /* AuNetRecState */
    uint8_t source;    /* AuFetchSource, or AU_NETREC_FROM_* */
    uint8_t error;     /* AuFetchError when failed */
    bool reused;       /* the connection was a pooled one */
    bool have_net;     /* the fetcher described the transfer (times, connection) */
    int status;        /* HTTP status; 0 before a response and for file: and data: */
    char type[48];     /* content type, "" when unknown */
    uint32_t transfer; /* body bytes as sent over the network; 0 from a cache */
    uint32_t decoded;  /* bytes delivered to the page */
    uint32_t connection; /* the connection's number; 0 when none */
    int redirects;
    char remote[56];   /* "93.184.215.14:443"; "" when unknown */
    char protocol[12]; /* "TLS 1.2"; "" without TLS */
    char cipher[48];
    /* Set by whatever refused the request: "mixed content", or the
     * content blocker's rule. "" when not blocked. */
    char blocked_reason[64];
    uint32_t start_ms; /* from the navigation */
    int32_t first_ms;  /* from start to the response head; -1 before it */
    int32_t total_ms;  /* from start to the end; -1 while pending */
    int32_t at[AU_FETCH_PHASE_DONE + 1]; /* AuFetchNet.at; all -1 without have_net */
    char *request_head;  /* NULL when not recorded */
    char *response_head; /* NULL when not recorded */
} AuNetRecord;

typedef struct AuNetLog {
    AuNetRecord *recs;  /* a ring of at most AU_NETLOG_MAX; it grows with the first requests */
    uint32_t cap;       /* records allocated */
    uint32_t first_seq; /* seq of the oldest record kept */
    uint32_t count;     /* records kept */
    uint32_t requests;  /* started since the navigation (count plus dropped) */
    uint32_t finished, failed, blocked, from_cache;
    uint32_t transfer_bytes, decoded_bytes; /* over every request, dropped ones too */
    uint32_t dcl_ms, load_ms; /* DOMContentLoaded and load, from the navigation; 0 before */
    uint32_t generation;      /* bumped by every change, for views */
} AuNetLog;

void au_netlog_init(AuNetLog *l);
void au_netlog_free(AuNetLog *l);
/* A new navigation: drops every record and the totals. */
void au_netlog_clear(AuNetLog *l);

/* A request started at start_ms (from the navigation). method NULL: GET.
 * Returns its seq, for the calls below; a seq whose record has left the
 * ring is ignored by them. */
uint32_t au_netlog_begin(AuNetLog *l, const char *url, const char *method, AuFetchKind kind, uint32_t start_ms);
void au_netlog_source(AuNetLog *l, uint32_t seq, int source);
/* The response head arrived at now_ms. */
void au_netlog_response(AuNetLog *l, uint32_t seq, int status, const char *type, bool from_cache, uint32_t now_ms);
/* len decoded bytes reached the page. */
void au_netlog_data(AuNetLog *l, uint32_t seq, size_t len);
/* The end at now_ms. state is DONE, FAILED, BLOCKED or CANCELLED; info
 * (may be NULL) gives the error, final status and type, and info->net
 * the transfer's description. */
void au_netlog_end(AuNetLog *l, uint32_t seq, AuNetRecState state, const AuFetchInfo *info, uint32_t now_ms);
void au_netlog_set_blocked_reason(AuNetLog *l, uint32_t seq, const char *reason);
void au_netlog_mark(AuNetLog *l, uint32_t dcl_ms, uint32_t load_ms); /* 0 leaves a value as it is */

/* Record i of count, oldest first; NULL out of range. */
const AuNetRecord *au_netlog_at(const AuNetLog *l, uint32_t i);

/* ---- Text ----
 * The Network window and `aura-host chrome --do netlog` show the same
 * text: fixed-width columns, one request per line. */
/* The column titles. */
void au_netlog_header(AuBuf *out);
/* One line (no line break): number, status, method, kind, transferred
 * and decoded size, time, source, connection, URL. */
void au_netlog_row(const AuNetRecord *r, AuBuf *out);
/* Everything about one request, one fact per line: URL, status, type,
 * source, connection and remote address, TLS, sizes, the phase timings,
 * why it was blocked or failed, and the request and response headers
 * when they were recorded. */
void au_netlog_detail(const AuNetRecord *r, AuBuf *out);
/* The totals in one line: requests, bytes, cache hits, failures,
 * DOMContentLoaded and load; connections ("" or NULL for none) is the
 * shell's text of the open connections per host and is appended. */
void au_netlog_footer(const AuNetLog *l, const char *connections, AuBuf *out);
/* Header, every row, footer; with details, each row is followed by its
 * detail lines, indented. */
void au_netlog_dump(const AuNetLog *l, const char *connections, bool details, AuBuf *out);

/* Writes a byte count as "512 B", "12.3 KB" or "4.56 MB". */
void au_put_size(AuBuf *out, uint32_t bytes);
/* Writes milliseconds as "412 ms" or "3.41 s". */
void au_put_ms(AuBuf *out, uint32_t ms);

#endif
