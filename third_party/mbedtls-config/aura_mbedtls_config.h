/* Aura's configuration of Mbed TLS (third_party/mbedtls).
 * third_party/mbedtls.cmake passes it as MBEDTLS_CONFIG_FILE. A TLS 1.2
 * and 1.3 client with X.509 verification: no server, no DTLS, no sockets,
 * no files, no renegotiation, no early data.
 *
 * AURA_MBEDTLS_SERVER (host builds only) adds the server side for the
 * in-process test server of tests/net/fakenet.c. The Mac OS 9 build never
 * defines it. */
#ifndef AURA_MBEDTLS_CONFIG_H
#define AURA_MBEDTLS_CONFIG_H

#define MBEDTLS_CONFIG_VERSION 0x04000000

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#ifdef AURA_MBEDTLS_SERVER
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_TICKET_C
#define MBEDTLS_SSL_CACHE_C
#endif
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED
/* Middlebox compatibility: the legacy session id and the ChangeCipherSpec
 * record that some network equipment expects to see. */
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_ALPN
#define MBEDTLS_SSL_SESSION_TICKETS
/* The library's TLS 1.3 code needs the peer's certificate kept (it does
 * not compile without this). The cost: the parsed leaf stays for the life
 * of the connection, and a saved TLS 1.2 session carries the leaf
 * certificate, 1 to 2.5 KB. */
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_ALL_ALERT_MESSAGES

#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT
/* Trust anchors are handed to the library by issuer name, one chain at a
 * time (roots.c), so the root store is never parsed as a whole. */
#define MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK
/* Text descriptions of certificates and verification results are not
 * used: tls.c has its own. */
#define MBEDTLS_X509_REMOVE_INFO

/* Incoming records may be 16 KB; tls.c writes at most 4 KB at a time. */
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

#endif
