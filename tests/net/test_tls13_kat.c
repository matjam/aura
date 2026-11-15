/* TLS 1.3 feasibility proof (docs/tls13-feasibility.md).
 *
 * Plays the client side of the RFC 8448 section 3 trace ("Simple 1-RTT
 * Handshake") with BearSSL 0.6 primitives only: X25519, SHA-256, HMAC,
 * AES-128-GCM, the X.509 decoder and the RSA public-key operation. It
 * derives every secret of the key schedule, opens the server's encrypted
 * flight, verifies the RSA-PSS CertificateVerify and the server Finished,
 * produces the client's Finished and application records byte for byte,
 * and derives the resumption PSK from the NewSessionTicket.
 *
 * What BearSSL 0.6 lacks, and this file supplies: HKDF-Expand from a
 * given PRK (br_hkdf_* only runs Extract then Expand in one context),
 * the TLS 1.3 record protection, and RSA-PSS verification (MGF1 over
 * br_rsa_i31_public). This is not a TLS client: nothing here is hardened
 * or meant for src/net as it stands.
 *
 * The vectors are in tls13_vectors.h (gen_tls13_vectors.py). */
#include "bearssl.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tls13_vectors.h"

static int failures;

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)
#define CHECK_MEM(a, b, n) CHECK(memcmp((a), (b), (n)) == 0)

#define HASH_MAX 64

static size_t hash_len(const br_hash_class *dig) { return (dig->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK; }

/* ---- HKDF (RFC 5869) on br_hmac ---- */

static void hkdf_extract(const br_hash_class *dig, const uint8_t *salt, size_t salt_len, const uint8_t *ikm,
                         size_t ikm_len, uint8_t *prk)
{
    br_hmac_key_context kc;
    br_hmac_context hc;
    br_hmac_key_init(&kc, dig, salt, salt_len);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, ikm, ikm_len);
    br_hmac_out(&hc, prk);
}

static void hkdf_expand(const br_hash_class *dig, const uint8_t *prk, const uint8_t *info, size_t info_len,
                        uint8_t *out, size_t out_len)
{
    size_t hl = hash_len(dig), done = 0;
    uint8_t t[HASH_MAX], ctr = 1;
    br_hmac_key_context kc;
    br_hmac_key_init(&kc, dig, prk, hl);
    while (done < out_len) {
        br_hmac_context hc;
        br_hmac_init(&hc, &kc, 0);
        if (ctr > 1)
            br_hmac_update(&hc, t, hl);
        br_hmac_update(&hc, info, info_len);
        br_hmac_update(&hc, &ctr, 1);
        br_hmac_out(&hc, t);
        size_t n = out_len - done < hl ? out_len - done : hl;
        memcpy(out + done, t, n);
        done += n;
        ctr++;
    }
}

/* HKDF-Expand-Label (RFC 8446 section 7.1). label without "tls13 ". */
static void expand_label(const br_hash_class *dig, const uint8_t *secret, const char *label, const uint8_t *ctx,
                         size_t ctx_len, uint8_t *out, size_t out_len)
{
    uint8_t info[2 + 1 + 6 + 32 + 1 + HASH_MAX];
    size_t ll = strlen(label), n = 0;
    info[n++] = (uint8_t)(out_len >> 8);
    info[n++] = (uint8_t)out_len;
    info[n++] = (uint8_t)(6 + ll);
    memcpy(info + n, "tls13 ", 6);
    n += 6;
    memcpy(info + n, label, ll);
    n += ll;
    info[n++] = (uint8_t)ctx_len;
    memcpy(info + n, ctx, ctx_len);
    n += ctx_len;
    hkdf_expand(dig, secret, info, n, out, out_len);
}

/* ---- Record protection (RFC 8446 section 5.2), AES-128-GCM ---- */

typedef struct Keys {
    br_aes_ct_ctr_keys aes;
    uint8_t iv[12];
    uint64_t seq;
} Keys;

static void keys_from_secret(Keys *k, const br_hash_class *dig, const uint8_t *secret)
{
    uint8_t key[16];
    expand_label(dig, secret, "key", NULL, 0, key, sizeof key);
    expand_label(dig, secret, "iv", NULL, 0, k->iv, sizeof k->iv);
    br_aes_ct_ctr_init(&k->aes, key, sizeof key);
    k->seq = 0;
}

static void nonce_for(const Keys *k, uint8_t nonce[12])
{
    memcpy(nonce, k->iv, 12);
    for (int i = 0; i < 8; i++)
        nonce[11 - i] ^= (uint8_t)(k->seq >> (8 * i));
}

/* Opens one record (header included) in place. Returns the content type
 * and sets *plain and *plain_len, or returns -1. */
static int record_open(Keys *k, uint8_t *rec, size_t rec_len, uint8_t **plain, size_t *plain_len)
{
    if (rec_len < 5 + 17 || rec[0] != 23 || rec[1] != 3 || rec[2] != 3)
        return -1;
    size_t len = ((size_t)rec[3] << 8) | rec[4];
    if (len != rec_len - 5 || len > 16384 + 256)
        return -1;
    uint8_t nonce[12];
    nonce_for(k, nonce);
    br_gcm_context g;
    br_gcm_init(&g, &k->aes.vtable, br_ghash_ctmul32);
    br_gcm_reset(&g, nonce, sizeof nonce);
    br_gcm_aad_inject(&g, rec, 5);
    br_gcm_flip(&g);
    br_gcm_run(&g, 0, rec + 5, len - 16);
    if (!br_gcm_check_tag(&g, rec + 5 + len - 16))
        return -1;
    k->seq++;
    size_t n = len - 16;
    while (n > 0 && rec[5 + n - 1] == 0) /* padding */
        n--;
    if (n == 0)
        return -1;
    *plain = rec + 5;
    *plain_len = n - 1;
    return rec[5 + n - 1];
}

/* Seals data of the given content type into out (5 + len + 1 + 16 bytes). */
static size_t record_seal(Keys *k, int type, const uint8_t *data, size_t len, uint8_t *out)
{
    size_t body = len + 1 + 16;
    out[0] = 23;
    out[1] = 3;
    out[2] = 3;
    out[3] = (uint8_t)(body >> 8);
    out[4] = (uint8_t)body;
    memcpy(out + 5, data, len);
    out[5 + len] = (uint8_t)type;
    uint8_t nonce[12];
    nonce_for(k, nonce);
    br_gcm_context g;
    br_gcm_init(&g, &k->aes.vtable, br_ghash_ctmul32);
    br_gcm_reset(&g, nonce, sizeof nonce);
    br_gcm_aad_inject(&g, out, 5);
    br_gcm_flip(&g);
    br_gcm_run(&g, 1, out + 5, len + 1);
    br_gcm_get_tag(&g, out + 5 + len + 1);
    k->seq++;
    return 5 + body;
}

/* ---- RSA-PSS verification (RFC 8017 section 9.1.2), salt length = hash length ---- */

static void mgf1_xor(const br_hash_class *dig, const uint8_t *seed, size_t seed_len, uint8_t *data, size_t len)
{
    size_t hl = hash_len(dig);
    for (uint32_t c = 0; len > 0; c++) {
        br_hash_compat_context hc;
        uint8_t cb[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c}, t[HASH_MAX];
        dig->init(&hc.vtable);
        dig->update(&hc.vtable, seed, seed_len);
        dig->update(&hc.vtable, cb, 4);
        dig->out(&hc.vtable, t);
        size_t n = len < hl ? len : hl;
        for (size_t i = 0; i < n; i++)
            data[i] ^= t[i];
        data += n;
        len -= n;
    }
}

/* True when sig is a valid RSASSA-PSS signature by pk over the message
 * whose hash (with dig) is mhash; MGF1 with dig, salt of hash length. */
static int rsa_pss_verify(const br_hash_class *dig, const uint8_t *mhash, const br_rsa_public_key *pk,
                          const uint8_t *sig, size_t sig_len)
{
    uint8_t em[512];
    const uint8_t *n = pk->n;
    size_t nlen = pk->nlen, hl = hash_len(dig);
    while (nlen > 0 && *n == 0) {
        n++;
        nlen--;
    }
    if (nlen == 0 || nlen > sizeof em || sig_len != nlen)
        return 0;
    unsigned top = 0;
    for (unsigned b = n[0]; b; b >>= 1)
        top++;
    size_t mod_bits = (nlen - 1) * 8 + top, em_bits = mod_bits - 1, em_len = (em_bits + 7) / 8;
    memcpy(em, sig, sig_len);
    br_rsa_public_key k = *pk;
    k.n = (unsigned char *)n;
    k.nlen = nlen;
    if (!br_rsa_i31_public(em, nlen, &k))
        return 0;
    uint8_t *e = em;
    if (em_len < nlen) { /* modulus bit length is 1 mod 8 */
        if (em[0] != 0)
            return 0;
        e++;
    }
    if (em_len < 2 * hl + 2 || e[em_len - 1] != 0xBC)
        return 0;
    size_t db_len = em_len - hl - 1;
    uint8_t *h = e + db_len;
    unsigned unused = (unsigned)(8 * em_len - em_bits);
    if (e[0] & (uint8_t)(0xFF00 >> unused))
        return 0;
    mgf1_xor(dig, h, hl, e, db_len);
    e[0] &= (uint8_t)(0xFF >> unused);
    size_t ps = db_len - hl - 1;
    uint8_t bad = 0;
    for (size_t i = 0; i < ps; i++)
        bad |= e[i];
    bad |= e[ps] ^ 0x01;
    if (bad)
        return 0;
    br_hash_compat_context hc;
    uint8_t zeros[8] = {0}, h2[HASH_MAX];
    dig->init(&hc.vtable);
    dig->update(&hc.vtable, zeros, 8);
    dig->update(&hc.vtable, mhash, hl);
    dig->update(&hc.vtable, e + ps + 1, hl);
    dig->out(&hc.vtable, h2);
    return memcmp(h, h2, hl) == 0;
}

static void hash_of(const br_hash_class *dig, const uint8_t *data, size_t len, uint8_t *out)
{
    br_hash_compat_context hc;
    dig->init(&hc.vtable);
    dig->update(&hc.vtable, data, len);
    dig->out(&hc.vtable, out);
}

static const uint8_t *find(const uint8_t *hay, size_t hay_len, const uint8_t *needle, size_t n)
{
    for (size_t i = 0; i + n <= hay_len; i++)
        if (!memcmp(hay + i, needle, n))
            return hay + i;
    return NULL;
}

/* ---- The RFC 8448 trace, client side ---- */

static void test_rfc8448(void)
{
    const br_hash_class *dig = &br_sha256_vtable;
    const uint8_t zeros[32] = {0};
    uint8_t th[32];

    /* Key share. br_ec_c25519_m31 takes the scalar and the point as
     * RFC 7748 writes them (little-endian), and clamps the scalar. */
    uint8_t pub[32], shared[32];
    const uint8_t *priv = v_client_priv;
    CHECK(br_ec_c25519_m31.mulgen(pub, priv, 32, BR_EC_curve25519) == 32);
    CHECK(find(v_client_hello, sizeof v_client_hello, pub, 32) != NULL);
    /* ServerHello: key_share extension 00 33 00 24 00 1d 00 20 <32> */
    static const uint8_t ks_head[] = {0x00, 0x33, 0x00, 0x24, 0x00, 0x1d, 0x00, 0x20};
    const uint8_t *ks = find(v_server_hello, sizeof v_server_hello, ks_head, sizeof ks_head);
    CHECK(ks != NULL);
    if (!ks)
        return;
    memcpy(shared, ks + sizeof ks_head, 32);
    CHECK(br_ec_c25519_m31.mul(shared, 32, priv, 32, BR_EC_curve25519) == 1);

    /* Key schedule to the handshake traffic secrets. */
    br_sha256_context tr;
    br_sha256_init(&tr);
    br_sha256_update(&tr, v_client_hello, sizeof v_client_hello);
    br_sha256_update(&tr, v_server_hello, sizeof v_server_hello);
    br_sha256_out(&tr, th);

    uint8_t empty_hash[32], early[32], derived[32], hs[32], c_hs[32], s_hs[32], master[32];
    hash_of(dig, NULL, 0, empty_hash);
    hkdf_extract(dig, zeros, 32, zeros, 32, early);
    expand_label(dig, early, "derived", empty_hash, 32, derived, 32);
    hkdf_extract(dig, derived, 32, shared, 32, hs);
    CHECK_MEM(hs, v_handshake_secret, 32);
    expand_label(dig, hs, "c hs traffic", th, 32, c_hs, 32);
    expand_label(dig, hs, "s hs traffic", th, 32, s_hs, 32);
    CHECK_MEM(c_hs, v_c_hs_traffic, 32);
    CHECK_MEM(s_hs, v_s_hs_traffic, 32);
    expand_label(dig, hs, "derived", empty_hash, 32, derived, 32);
    hkdf_extract(dig, derived, 32, zeros, 32, master);
    CHECK_MEM(master, v_master_secret, 32);

    Keys s_hs_keys, c_hs_keys;
    keys_from_secret(&s_hs_keys, dig, s_hs);
    keys_from_secret(&c_hs_keys, dig, c_hs);
    CHECK_MEM(s_hs_keys.iv, v_s_hs_iv, 12);

    /* The server's encrypted flight: EncryptedExtensions, Certificate,
     * CertificateVerify, Finished in one record. */
    uint8_t flight[sizeof v_rec_server_flight];
    memcpy(flight, v_rec_server_flight, sizeof flight);
    flight[100] ^= 1; /* a damaged record must not open */
    uint8_t *p;
    size_t plen;
    CHECK(record_open(&s_hs_keys, flight, sizeof flight, &p, &plen) == -1);
    memcpy(flight, v_rec_server_flight, sizeof flight);
    int type = record_open(&s_hs_keys, flight, sizeof flight, &p, &plen);
    CHECK(type == 22);
    CHECK(plen == 657);
    if (type != 22)
        return;

    static const int order[4] = {8, 11, 15, 20};
    int seen = 0;
    const uint8_t *cert = NULL, *sig = NULL;
    size_t cert_len = 0, sig_len = 0, off = 0;
    unsigned sig_alg = 0;
    while (off + 4 <= plen && seen < 4) {
        const uint8_t *m = p + off;
        size_t mlen = ((size_t)m[1] << 16) | ((size_t)m[2] << 8) | m[3];
        CHECK(off + 4 + mlen <= plen);
        CHECK(m[0] == order[seen]);
        const uint8_t *b = m + 4;
        if (m[0] == 11) {
            /* context(1) = 0, list length(3), certificate length(3), DER, extensions(2) */
            CHECK(mlen > 9 && b[0] == 0);
            cert_len = ((size_t)b[4] << 16) | ((size_t)b[5] << 8) | b[6];
            cert = b + 7;
            CHECK(7 + cert_len + 2 <= mlen);
        } else if (m[0] == 15) {
            /* The signature covers the transcript up to Certificate. */
            br_sha256_out(&tr, th);
            sig_alg = ((unsigned)b[0] << 8) | b[1];
            sig_len = ((size_t)b[2] << 8) | b[3];
            sig = b + 4;
            CHECK(4 + sig_len == mlen);
            CHECK(sig_alg == 0x0804); /* rsa_pss_rsae_sha256 */
            uint8_t content[64 + 33 + 1 + 32], chash[32];
            memset(content, 0x20, 64);
            memcpy(content + 64, "TLS 1.3, server CertificateVerify", 33);
            content[97] = 0;
            memcpy(content + 98, th, 32);
            hash_of(dig, content, sizeof content, chash);
            br_x509_decoder_context dc;
            br_x509_decoder_init(&dc, NULL, NULL);
            br_x509_decoder_push(&dc, cert, cert_len);
            const br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
            CHECK(pk != NULL && pk->key_type == BR_KEYTYPE_RSA);
            if (pk && pk->key_type == BR_KEYTYPE_RSA) {
                CHECK(rsa_pss_verify(dig, chash, &pk->key.rsa, sig, sig_len) == 1);
                uint8_t bad[512];
                memcpy(bad, sig, sig_len);
                bad[sig_len / 2] ^= 0x10;
                CHECK(rsa_pss_verify(dig, chash, &pk->key.rsa, bad, sig_len) == 0);
                chash[0] ^= 1;
                CHECK(rsa_pss_verify(dig, chash, &pk->key.rsa, sig, sig_len) == 0);
            }
        } else if (m[0] == 20) {
            /* verify_data = HMAC(finished_key, transcript up to CertificateVerify) */
            uint8_t fk[32], vd[32];
            br_hmac_key_context kc;
            br_hmac_context hc;
            br_sha256_out(&tr, th);
            expand_label(dig, s_hs, "finished", NULL, 0, fk, 32);
            br_hmac_key_init(&kc, dig, fk, 32);
            br_hmac_init(&hc, &kc, 0);
            br_hmac_update(&hc, th, 32);
            br_hmac_out(&hc, vd);
            CHECK(mlen == 32);
            CHECK_MEM(vd, b, 32);
        }
        br_sha256_update(&tr, m, 4 + mlen);
        off += 4 + mlen;
        seen++;
    }
    CHECK(seen == 4 && off == plen);

    /* Application traffic secrets: transcript through the server Finished. */
    uint8_t c_ap[32], s_ap[32];
    br_sha256_out(&tr, th);
    expand_label(dig, master, "c ap traffic", th, 32, c_ap, 32);
    expand_label(dig, master, "s ap traffic", th, 32, s_ap, 32);
    CHECK_MEM(c_ap, v_c_ap_traffic, 32);
    CHECK_MEM(s_ap, v_s_ap_traffic, 32);

    /* The client's Finished, sealed under the client handshake keys,
     * must equal the trace's record byte for byte. */
    uint8_t fin[36] = {20, 0, 0, 32}, fk[32], rec[128];
    br_hmac_key_context kc;
    br_hmac_context hc;
    expand_label(dig, c_hs, "finished", NULL, 0, fk, 32);
    br_hmac_key_init(&kc, dig, fk, 32);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, th, 32);
    br_hmac_out(&hc, fin + 4);
    size_t rl = record_seal(&c_hs_keys, 22, fin, sizeof fin, rec);
    CHECK(rl == sizeof v_rec_client_finished);
    CHECK_MEM(rec, v_rec_client_finished, sizeof v_rec_client_finished);
    br_sha256_update(&tr, fin, sizeof fin);

    /* Resumption: the ticket arrives under the server application keys. */
    Keys s_ap_keys, c_ap_keys;
    keys_from_secret(&s_ap_keys, dig, s_ap);
    keys_from_secret(&c_ap_keys, dig, c_ap);
    uint8_t res[32], psk[32], tick[sizeof v_rec_ticket];
    br_sha256_out(&tr, th);
    expand_label(dig, master, "res master", th, 32, res, 32);
    CHECK_MEM(res, v_res_master, 32);
    memcpy(tick, v_rec_ticket, sizeof tick);
    type = record_open(&s_ap_keys, tick, sizeof tick, &p, &plen);
    CHECK(type == 22 && plen > 13 && p[0] == 4);
    if (type == 22 && plen > 13) {
        /* lifetime(4) age_add(4) nonce(1+n) ticket(2+n) extensions(2+n) */
        size_t nonce_len = p[4 + 8];
        expand_label(dig, res, "resumption", p + 4 + 9, nonce_len, psk, 32);
        CHECK_MEM(psk, v_resumption_psk, 32);
    }

    /* Application data both ways, then the server's close_notify. */
    rl = record_seal(&c_ap_keys, 23, v_app_payload, sizeof v_app_payload, rec);
    CHECK(rl == sizeof v_rec_client_app);
    CHECK_MEM(rec, v_rec_client_app, sizeof v_rec_client_app);
    uint8_t app[sizeof v_rec_server_app], alert[sizeof v_rec_server_alert];
    memcpy(app, v_rec_server_app, sizeof app);
    type = record_open(&s_ap_keys, app, sizeof app, &p, &plen);
    CHECK(type == 23 && plen == sizeof v_app_payload);
    if (type == 23)
        CHECK_MEM(p, v_app_payload, sizeof v_app_payload);
    memcpy(alert, v_rec_server_alert, sizeof alert);
    type = record_open(&s_ap_keys, alert, sizeof alert, &p, &plen);
    CHECK(type == 21 && plen == 2 && p[0] == 1 && p[1] == 0);
}

/* ---- RSA-PSS with keys of the sizes servers use ---- */

static void test_pss(void)
{
    static const unsigned char e[3] = {1, 0, 1};
    uint8_t mh[HASH_MAX], bad[512];
    br_rsa_public_key pk;

    pk.n = (unsigned char *)v_pss_sha256_mod;
    pk.nlen = sizeof v_pss_sha256_mod;
    pk.e = (unsigned char *)e;
    pk.elen = 3;
    hash_of(&br_sha256_vtable, v_pss_msg, sizeof v_pss_msg, mh);
    CHECK(rsa_pss_verify(&br_sha256_vtable, mh, &pk, v_pss_sha256_sig, sizeof v_pss_sha256_sig) == 1);
    for (size_t i = 0; i < sizeof v_pss_sha256_sig; i += 37) {
        memcpy(bad, v_pss_sha256_sig, sizeof v_pss_sha256_sig);
        bad[i] ^= 0x04;
        CHECK(rsa_pss_verify(&br_sha256_vtable, mh, &pk, bad, sizeof v_pss_sha256_sig) == 0);
    }
    /* The right signature under the wrong hash function. */
    CHECK(rsa_pss_verify(&br_sha384_vtable, mh, &pk, v_pss_sha256_sig, sizeof v_pss_sha256_sig) == 0);

    pk.n = (unsigned char *)v_pss_sha384_mod;
    pk.nlen = sizeof v_pss_sha384_mod;
    hash_of(&br_sha384_vtable, v_pss_msg, sizeof v_pss_msg, mh);
    CHECK(rsa_pss_verify(&br_sha384_vtable, mh, &pk, v_pss_sha384_sig, sizeof v_pss_sha384_sig) == 1);
    mh[5] ^= 1;
    CHECK(rsa_pss_verify(&br_sha384_vtable, mh, &pk, v_pss_sha384_sig, sizeof v_pss_sha384_sig) == 0);
}

int main(void)
{
    test_rfc8448();
    test_pss();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
