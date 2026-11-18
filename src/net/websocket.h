/* The WebSocket protocol (RFC 6455) for the client side: the opening
 * handshake's key and accept values, and framing. Sans I/O: the network
 * layer (http.c) carries the handshake and then the raw bytes; script's
 * WebSocket (src/js/websocket.js) feeds received bytes to a parser and
 * builds the frames it sends.
 *
 *     AuWsParser p;
 *     au_ws_parser_init(&p, max_message);
 *     au_ws_feed(&p, bytes, len, &callbacks);   // as often as data comes
 *     au_ws_parser_free(&p);
 *
 * What the parser checks, as the RFC says (a failure is reported once
 * through error() with the close code to send, and the parser then
 * ignores everything after it):
 * - a server's frames are not masked (1002);
 * - no RSV bit is set: Aura offers no extensions (1002);
 * - the opcode is known (1002);
 * - control frames (close, ping, pong) have FIN set and at most 125
 *   bytes, and may come between the fragments of a message (1002);
 * - a continuation frame continues a message, and a new text or binary
 *   frame does not start inside one (1002);
 * - the 64-bit length has its top bit clear (1002);
 * - a text message, and a close frame's reason, are UTF-8 (1007);
 * - a close frame's body is empty or at least 2 bytes, with a code a
 *   peer may send (1002);
 * - a message longer than max_message bytes fails with 1009.
 *
 * Frames a client sends are always masked with the 4-byte key the caller
 * gives; the caller takes it from a strong random source for every
 * frame (RFC 6455 section 10.3). */
#ifndef AURA_NET_WEBSOCKET_H
#define AURA_NET_WEBSOCKET_H

#include "../core/core.h"

enum {
    AU_WS_CONT = 0x0,
    AU_WS_TEXT = 0x1,
    AU_WS_BINARY = 0x2,
    AU_WS_CLOSE = 0x8,
    AU_WS_PING = 0x9,
    AU_WS_PONG = 0xA
};

/* The base64 of 16 nonce bytes: a Sec-WebSocket-Key (24 characters). */
void au_ws_key(const uint8_t nonce[16], char out[25]);
/* The Sec-WebSocket-Accept a server must answer key with: base64 of the
 * SHA-1 of the key and the RFC's GUID (28 characters). False when the
 * digest is not available. */
bool au_ws_accept(const char *key, char out[29]);

typedef struct AuWsCallbacks {
    void *ctx;
    /* A complete message: AU_WS_TEXT (valid UTF-8) or AU_WS_BINARY. */
    void (*message)(void *ctx, int opcode, const uint8_t *data, size_t len);
    /* A control frame: AU_WS_PING or AU_WS_PONG with its payload. */
    void (*control)(void *ctx, int opcode, const uint8_t *data, size_t len);
    /* A close frame: code 1005 when it had no body; reason is UTF-8. */
    void (*close)(void *ctx, uint16_t code, const char *reason, size_t reason_len);
    /* The stream is broken: code is the close code to send (1002, 1007,
     * 1009) and why says what was wrong, for the console. */
    void (*error)(void *ctx, uint16_t code, const char *why);
} AuWsCallbacks;

typedef struct AuWsParser {
    uint8_t head[14]; /* the frame header so far */
    size_t head_len, head_need;
    bool in_payload;
    bool fin;
    int opcode;           /* of the frame being read */
    uint64_t remaining;   /* payload bytes of the frame still to come */
    int msg_opcode;       /* AU_WS_TEXT or AU_WS_BINARY while a message is fragmented, else 0 */
    AuBuf msg;            /* the data message so far */
    uint8_t ctrl[125];    /* the control frame's payload */
    size_t ctrl_len;
    size_t max_message;
    bool failed, closed;  /* closed: a close frame came; nothing after it counts */
} AuWsParser;

void au_ws_parser_init(AuWsParser *p, size_t max_message);
void au_ws_parser_free(AuWsParser *p);
void au_ws_feed(AuWsParser *p, const void *bytes, size_t len, const AuWsCallbacks *cb);

/* Appends one frame with FIN set, masked with mask, to out. */
void au_ws_frame(AuBuf *out, int opcode, const void *data, size_t len, const uint8_t mask[4]);
/* Whether a text payload is UTF-8 (U+FFFD itself allowed). */
bool au_ws_utf8_ok(const uint8_t *s, size_t n);

#endif
