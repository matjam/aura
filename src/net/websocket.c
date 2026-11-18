/* The WebSocket protocol for the client side (see websocket.h). */
#include "websocket.h"
#include "auth.h"
#include "digest.h"

#include <string.h>

static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

void au_ws_key(const uint8_t nonce[16], char out[25])
{
    AuBuf b;
    au_buf_init(&b);
    au_base64_encode(nonce, 16, &b);
    size_t n = b.len < 24 ? b.len : 24;
    memcpy(out, b.data, n);
    out[n] = 0;
    au_buf_free(&b);
}

bool au_ws_accept(const char *key, char out[29])
{
    const void *parts[2] = {key, GUID};
    size_t lens[2] = {strlen(key), sizeof GUID - 1};
    uint8_t d[AU_DIGEST_MAX];
    out[0] = 0;
    if (!au_digest_parts(AU_DIGEST_SHA1, parts, lens, 2, d))
        return false;
    AuBuf b;
    au_buf_init(&b);
    au_base64_encode(d, 20, &b);
    size_t n = b.len < 28 ? b.len : 28;
    memcpy(out, b.data, n);
    out[n] = 0;
    au_buf_free(&b);
    return n == 28;
}

bool au_ws_utf8_ok(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (s[i] < 0x80) {
            i++;
            continue;
        }
        size_t adv;
        uint32_t cp = au_utf8_decode((const char *)s + i, n - i, &adv);
        if (cp == 0xFFFD && adv == 1)
            return false;
        i += adv;
    }
    return true;
}

void au_ws_parser_init(AuWsParser *p, size_t max_message)
{
    memset(p, 0, sizeof *p);
    au_buf_init(&p->msg);
    p->head_need = 2;
    p->max_message = max_message;
}

void au_ws_parser_free(AuWsParser *p)
{
    au_buf_free(&p->msg);
    memset(p, 0, sizeof *p);
}

static void fail(AuWsParser *p, const AuWsCallbacks *cb, uint16_t code, const char *why)
{
    if (p->failed)
        return;
    p->failed = true;
    au_buf_free(&p->msg);
    if (cb->error)
        cb->error(cb->ctx, code, why);
}

static bool control_op(int op) { return op >= 0x8; }

/* The frame header is complete: checks it and starts the payload. */
static void begin_frame(AuWsParser *p, const AuWsCallbacks *cb)
{
    uint8_t b0 = p->head[0], b1 = p->head[1];
    p->fin = (b0 & 0x80) != 0;
    p->opcode = b0 & 0x0F;
    uint64_t len = b1 & 0x7F;
    if (len == 126)
        len = ((uint64_t)p->head[2] << 8) | p->head[3];
    else if (len == 127) {
        len = 0;
        for (int i = 0; i < 8; i++)
            len = (len << 8) | p->head[2 + i];
    }
    if (b0 & 0x70) {
        fail(p, cb, 1002, "a frame has a reserved bit set");
        return;
    }
    if (b1 & 0x80) {
        fail(p, cb, 1002, "a frame from the server is masked");
        return;
    }
    switch (p->opcode) {
    case AU_WS_CONT:
    case AU_WS_TEXT:
    case AU_WS_BINARY:
    case AU_WS_CLOSE:
    case AU_WS_PING:
    case AU_WS_PONG: break;
    default: fail(p, cb, 1002, "a frame has an unknown opcode"); return;
    }
    if ((b1 & 0x7F) == 127 && (p->head[2] & 0x80)) {
        fail(p, cb, 1002, "a frame's length is out of range");
        return;
    }
    if (control_op(p->opcode)) {
        if (!p->fin || len > 125) {
            fail(p, cb, 1002, "a control frame is fragmented or longer than 125 bytes");
            return;
        }
        p->ctrl_len = 0;
    } else {
        if (p->opcode == AU_WS_CONT && !p->msg_opcode) {
            fail(p, cb, 1002, "a continuation frame continues no message");
            return;
        }
        if (p->opcode != AU_WS_CONT && p->msg_opcode) {
            fail(p, cb, 1002, "a new message starts inside a fragmented one");
            return;
        }
        if (len > (uint64_t)p->max_message || p->msg.len + (size_t)len > p->max_message) {
            fail(p, cb, 1009, "a message is too large");
            return;
        }
        if (p->opcode != AU_WS_CONT)
            p->msg_opcode = p->opcode;
    }
    p->remaining = len;
    p->in_payload = true;
}

static bool close_code_ok(uint16_t c)
{
    if (c >= 3000 && c <= 4999)
        return true;
    return (c >= 1000 && c <= 1003) || (c >= 1007 && c <= 1014);
}

/* The frame's payload is complete. */
static void end_frame(AuWsParser *p, const AuWsCallbacks *cb)
{
    p->in_payload = false;
    p->head_len = 0;
    p->head_need = 2;
    if (control_op(p->opcode)) {
        if (p->opcode == AU_WS_CLOSE) {
            uint16_t code = 1005;
            const char *reason = "";
            size_t rl = 0;
            if (p->ctrl_len == 1) {
                fail(p, cb, 1002, "a close frame has a 1-byte body");
                return;
            }
            if (p->ctrl_len >= 2) {
                code = (uint16_t)((p->ctrl[0] << 8) | p->ctrl[1]);
                if (!close_code_ok(code)) {
                    fail(p, cb, 1002, "a close frame has a code a peer may not send");
                    return;
                }
                reason = (const char *)p->ctrl + 2;
                rl = p->ctrl_len - 2;
                if (!au_ws_utf8_ok(p->ctrl + 2, rl)) {
                    fail(p, cb, 1007, "a close frame's reason is not UTF-8");
                    return;
                }
            }
            p->closed = true;
            char r[124];
            memcpy(r, reason, rl);
            r[rl] = 0;
            if (cb->close)
                cb->close(cb->ctx, code, r, rl);
            return;
        }
        if (cb->control)
            cb->control(cb->ctx, p->opcode, p->ctrl, p->ctrl_len);
        return;
    }
    if (!p->fin)
        return;
    int op = p->msg_opcode;
    p->msg_opcode = 0;
    if (op == AU_WS_TEXT && !au_ws_utf8_ok((const uint8_t *)p->msg.data, p->msg.len)) {
        fail(p, cb, 1007, "a text message is not UTF-8");
        return;
    }
    if (cb->message)
        cb->message(cb->ctx, op, (const uint8_t *)(p->msg.len ? p->msg.data : ""), p->msg.len);
    au_buf_clear(&p->msg);
}

void au_ws_feed(AuWsParser *p, const void *bytes, size_t len, const AuWsCallbacks *cb)
{
    const uint8_t *d = bytes;
    size_t i = 0;
    while (!p->failed && !p->closed) {
        if (!p->in_payload) {
            while (i < len && p->head_len < p->head_need) {
                p->head[p->head_len++] = d[i++];
                if (p->head_len == 2) {
                    uint8_t l7 = p->head[1] & 0x7F;
                    p->head_need = 2 + (l7 == 126 ? 2 : l7 == 127 ? 8 : 0) + ((p->head[1] & 0x80) ? 4 : 0);
                }
            }
            if (p->head_len < p->head_need)
                return;
            begin_frame(p, cb);
            if (p->failed)
                return;
            if (p->remaining == 0) {
                end_frame(p, cb);
                continue;
            }
        }
        if (i >= len)
            return;
        size_t take = len - i;
        if ((uint64_t)take > p->remaining)
            take = (size_t)p->remaining;
        if (control_op(p->opcode)) {
            memcpy(p->ctrl + p->ctrl_len, d + i, take);
            p->ctrl_len += take;
        } else {
            au_buf_append(&p->msg, d + i, take);
        }
        i += take;
        p->remaining -= take;
        if (p->remaining == 0)
            end_frame(p, cb);
    }
}

void au_ws_frame(AuBuf *out, int opcode, const void *data, size_t len, const uint8_t mask[4])
{
    uint8_t h[14];
    size_t hl = 0;
    h[hl++] = (uint8_t)(0x80 | (opcode & 0x0F));
    if (len < 126)
        h[hl++] = (uint8_t)(0x80 | len);
    else if (len <= 0xFFFF) {
        h[hl++] = 0x80 | 126;
        h[hl++] = (uint8_t)(len >> 8);
        h[hl++] = (uint8_t)len;
    } else {
        h[hl++] = 0x80 | 127;
        uint64_t l = len;
        for (int i = 7; i >= 0; i--)
            h[hl++] = (uint8_t)(l >> (8 * i));
    }
    memcpy(h + hl, mask, 4);
    hl += 4;
    au_buf_append(out, h, hl);
    size_t start = out->len;
    au_buf_append(out, data, len);
    uint8_t *p = (uint8_t *)out->data + start;
    for (size_t i = 0; i < len; i++)
        p[i] ^= mask[i & 3];
}
