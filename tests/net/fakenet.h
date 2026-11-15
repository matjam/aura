/* An in-process fake of AuNetOps for the network tests: scripted servers
 * by host name, a fake clock and an entropy source. No real network.
 *
 * Each server answers requests in order from its script. A request is
 * complete after the blank line and its Content-Length or chunked body.
 * A response is delivered in pieces of `chunk` bytes per recv (0: all at
 * once) and may close or reset the connection afterwards, or never come
 * (silent). With tls set, the server side runs a BearSSL server engine on
 * the connection, with the given chain and EC key. */
#ifndef AURA_TEST_FAKENET_H
#define AURA_TEST_FAKENET_H

#include "net/net.h"

#include "bearssl.h"

#define FAKE_MAX_RESP 32
#define FAKE_MAX_SERVERS 16
#define FAKE_MAX_SOCKS 64

typedef struct FakeResponse {
    const char *data;
    size_t len;          /* 0: strlen(data) */
    size_t chunk;        /* bytes per recv; 0 for all */
    bool close_after;    /* orderly close after the response */
    bool reset_after;    /* error on the next recv after the response */
    bool silent;         /* never answer */
    bool close_before;   /* close without answering (stale keep-alive) */
} FakeResponse;

typedef struct FakeServer {
    char host[64];
    uint16_t port;
    bool dns_fail, refuse, connect_hang;
    const char *banner; /* sent as soon as a (non-TLS) connection opens */
    FakeResponse resp[FAKE_MAX_RESP];
    int nresp, next;
    /* What the server received, one entry per request. */
    char *requests[FAKE_MAX_RESP];
    int nreq;
    int connections;
    /* TLS */
    bool tls;
    const br_x509_certificate *chain;
    size_t chain_len;
    const br_ec_private_key *eckey;
    /* Session cache, so clients can resume (set up on first connect). */
    br_ssl_session_cache_lru cache;
    unsigned char cache_buf[4096];
    bool cache_ready;
} FakeServer;

typedef struct FakeNet {
    AuNetOps ops;
    FakeServer servers[FAKE_MAX_SERVERS];
    int nservers;
    uint32_t now;
    int64_t wall;
    bool entropy_ready;
    uint32_t entropy_counter;
    struct FakeSock *socks[FAKE_MAX_SOCKS];
    uint32_t resolve_next;
    int open_socks;
} FakeNet;

void fake_init(FakeNet *f);
void fake_free(FakeNet *f);
FakeServer *fake_server(FakeNet *f, const char *host, uint16_t port);
void fake_respond(FakeServer *s, const char *data);
FakeResponse *fake_add(FakeServer *s, const char *data);
/* Pumps net until it is idle or max_rounds pass, advancing the clock by
 * step_ms per round. Returns the rounds used. */
int fake_run(FakeNet *f, AuNet *net, int max_rounds, uint32_t step_ms);

#endif
