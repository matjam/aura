/* An in-process fake of AuNetOps for the network tests: scripted servers
 * by host name, a fake clock and an entropy source. No real network.
 *
 * Each server answers requests in order from its script. A request is
 * complete after the blank line and its Content-Length or chunked body.
 * A response is delivered in pieces of `chunk` bytes per recv (0: all at
 * once) and may close or reset the connection afterwards, or never come
 * (silent). With tls set, the server side runs the TLS library's server
 * (Mbed TLS, which the host build configures with its server half) on the
 * connection, with the given chain and key, TLS 1.2 and 1.3 unless
 * tls_min or tls_max say otherwise. */
#ifndef AURA_TEST_FAKENET_H
#define AURA_TEST_FAKENET_H

#include "net/net.h"

#include "tls_fixtures.h"

#define FAKE_MAX_RESP 32
#define FAKE_MAX_SERVERS 16
#define FAKE_MAX_SOCKS 64

typedef struct FakeResponse {
    const char *data;
    size_t len;          /* 0: strlen(data) */
    size_t chunk;        /* bytes per recv; 0 for all */
    bool paced;          /* plain connections: one chunk per clock tick (FakeNet.now), as a slow link */
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
    const TlsCert *chain;
    size_t chain_len;
    const TlsKey *key; /* the leaf's private key */
    /* 12 or 13 to set the lowest or highest protocol version; 0 leaves
     * TLS 1.2 as the lowest and TLS 1.3 as the highest. */
    int tls_min, tls_max;
    /* The session cache and ticket keys, so clients can resume (made on
     * the first connection). */
    struct FakeTlsServer *tls_state;
    bool alpn_http11; /* the server accepts the ALPN protocol "http/1.1" */
    /* RSA key exchange (TLS 1.2, no forward secrecy): 1, the server offers
     * only that, as some old servers do; 2, it offers ECDHE first and RSA
     * key exchange too; 0, ECDHE only. */
    int rsa_kex;
    bool no_resume; /* no session cache and no tickets: every handshake is a full one */
    /* The server accepts only P-256 key shares, so a client that sends an
     * X25519 share gets a HelloRetryRequest. */
    bool p256_only;
    /* An attacker in the path: the TLS 1.3 entry of the ClientHello's
     * supported_versions is rewritten to TLS 1.2 before the server sees
     * it, so the server negotiates TLS 1.2 and marks its random with the
     * downgrade sentinel. */
    bool strip_tls13;
    /* When set, called with each piece of TLS ciphertext on its way to
     * the client; it may change the bytes (a fuzzer, a faulty link). */
    void (*mangle)(void *ctx, uint8_t *data, size_t len);
    void *mangle_ctx;
    int handshakes;       /* TLS handshakes completed */
    int last_version;     /* 12 or 13: the last completed handshake's version */
    bool last_hrr;        /* that handshake's first ClientHello was answered with a HelloRetryRequest */
    char last_suite[64];  /* the library's name of the cipher suite */
    /* With port 0 (any port): when set, tls, banner and connect_hang apply
     * only to connections on this port, so one host can serve plain http
     * on 80 and a broken https on 443. */
    uint16_t special_port;
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
/* For tests that load a root store and nothing else: starts the TLS
 * library's random generator with a fixed seed (fake_init does it too). */
void fake_tls_start(void);
FakeServer *fake_server(FakeNet *f, const char *host, uint16_t port);
void fake_respond(FakeServer *s, const char *data);
FakeResponse *fake_add(FakeServer *s, const char *data);
/* Pumps net until it is idle or max_rounds pass, advancing the clock by
 * step_ms per round. Returns the rounds used. */
int fake_run(FakeNet *f, AuNet *net, int max_rounds, uint32_t step_ms);

#endif
