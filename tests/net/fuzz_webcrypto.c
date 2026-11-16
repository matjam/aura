/* Fuzzes crypto.subtle's C side (src/net/webcrypto.c): mutated key data
 * imported as every kind of key (RSA PKCS #1 DER, EC scalars and points,
 * X25519), and each key that imports used for what its kind allows
 * (sign, verify, encrypt, decrypt, agree, export); AES in all four modes
 * and the two key derivations with random lengths and parameters. The
 * library must refuse what is malformed without reading or writing past
 * a buffer; run it under the sanitizers.
 *
 *   fuzz_webcrypto ITERATIONS SEED SECONDS
 *   fuzz_webcrypto replay FILE...
 *
 * The seeds are keys generated at start (an RSA-1024 pair, P-256, P-384,
 * P-521 and X25519 keys) and exported. A replayed file is imported as
 * every kind of key. */
#include "../fuzz.h"
#include "net/webcrypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FzCorpus corpus;
static unsigned imported, failed_ops;

static AuDigestAlg any_hash(void) { return (AuDigestAlg)(1 + fz_rnd(4)); }

/* Uses a key that imported: every operation its kind has, on random data
 * with random lengths; results are checked against the lengths promised. */
static void exercise(AuWcKey *k, AuWcKey *peer)
{
    uint8_t data[300], out[1200];
    size_t n = fz_rnd(sizeof data), got = 0;
    for (size_t i = 0; i < n; i++)
        data[i] = (uint8_t)fz_next();
    size_t elen = 0;
    uint8_t *e = au_wc_export(k, fz_rnd(2) != 0, &elen);
    au_wc_wipe_free(e, elen);
    AuDigestAlg h = any_hash();
    switch (au_wc_key_kind(k)) {
    case AU_WC_RSA:
    case AU_WC_EC: {
        size_t sl = au_wc_sig_len(k);
        if (sl > sizeof out)
            abort();
        for (int alg = AU_WC_RSASSA; alg <= AU_WC_ECDSA; alg++) {
            if (au_wc_sign(k, (AuWcAlg)alg, h, data, n, out, sizeof out, &got) == AU_WC_OK && got > sl)
                abort();
            bool ok;
            size_t siglen = fz_rnd(2) ? got : fz_rnd(sizeof out);
            if (fz_rnd(3) == 0 && siglen)
                out[fz_rnd((uint32_t)siglen)] ^= 1;
            if (au_wc_verify(k, (AuWcAlg)alg, h, fz_rnd(2) != 0, data, n, out, siglen, &ok) != AU_WC_OK)
                failed_ops++;
        }
        (void)au_wc_pss_salt(k, h);
        uint8_t ct[600];
        size_t cl = 0;
        if (au_wc_oaep(k, false, h, data, fz_rnd(20), data, fz_rnd((uint32_t)n + 1), ct, sizeof ct, &cl) == AU_WC_OK) {
            if (cl > sizeof ct)
                abort();
            if (fz_rnd(2) && cl)
                ct[fz_rnd((uint32_t)cl)] ^= 0x80;
            au_wc_oaep(k, true, h, data, fz_rnd(20), ct, fz_rnd(2) ? cl : fz_rnd(sizeof ct), out, sizeof out, &got);
        }
        if (peer && au_wc_agree(k, peer, out, sizeof out, &got) == AU_WC_OK && got > 66)
            abort();
        break;
    }
    case AU_WC_X25519:
        if (peer && au_wc_agree(k, peer, out, sizeof out, &got) == AU_WC_OK && got != 32)
            abort();
        break;
    default:
        break;
    }
}

static void import_all(const uint8_t *d, size_t n)
{
    static const int curves[] = {256, 384, 521};
    AuWcKey *keys[16];
    int nk = 0;
    for (int priv = 0; priv < 2; priv++) {
        for (int alg = AU_WC_RSASSA; alg <= AU_WC_OAEP; alg++) {
            AuWcKey *k = NULL;
            if (au_wc_import(AU_WC_RSA, priv != 0, 0, (AuWcAlg)alg, any_hash(), d, n, &k) == AU_WC_OK)
                keys[nk++] = k;
        }
        for (int c = 0; c < 3; c++) {
            AuWcKey *k = NULL;
            if (au_wc_import(AU_WC_EC, priv != 0, curves[c], fz_rnd(2) ? AU_WC_ECDSA : AU_WC_ECDH, (AuDigestAlg)0, d,
                             n, &k) == AU_WC_OK)
                keys[nk++] = k;
        }
        AuWcKey *k = NULL;
        if (au_wc_import(AU_WC_X25519, priv != 0, 0, AU_WC_ECDH, (AuDigestAlg)0, d, n, &k) == AU_WC_OK)
            keys[nk++] = k;
    }
    imported += (unsigned)nk;
    for (int i = 0; i < nk; i++)
        exercise(keys[i], keys[fz_rnd((uint32_t)nk)]);
    for (int i = 0; i < nk; i++)
        au_wc_key_free(keys[i]);
}

/* AES and the key derivations with random lengths and parameters. */
static void symmetric(void)
{
    static uint8_t in[2100], out[2200 + 32];
    uint8_t kb[40], iv[40], aad[40];
    size_t kl = fz_rnd(4) ? 16 + 8 * fz_rnd(3) : fz_rnd(sizeof kb);
    for (size_t i = 0; i < sizeof kb; i++)
        kb[i] = (uint8_t)fz_next(), iv[i] = (uint8_t)fz_next(), aad[i] = (uint8_t)fz_next();
    AuWcKey *k = au_wc_secret(kb, kl);
    size_t n = fz_rnd(4) ? fz_rnd(80) : fz_rnd(sizeof in);
    for (size_t i = 0; i < n; i++)
        in[i] = (uint8_t)fz_next();
    AuWcAes mode = (AuWcAes)(1 + fz_rnd(4));
    bool dec = fz_rnd(2) != 0;
    unsigned param = mode == AU_WC_GCM ? (fz_rnd(4) ? 16 : fz_rnd(20)) : mode == AU_WC_CTR ? fz_rnd(132) : 0;
    size_t ivl = mode == AU_WC_CBC || mode == AU_WC_CTR ? (fz_rnd(5) ? 16 : fz_rnd(sizeof iv)) : fz_rnd(sizeof iv);
    size_t max = au_wc_aes_out_max(mode, dec, n, param), got = 0;
    if (max > sizeof out - 32)
        abort();
    AuWcErr err = au_wc_aes(k, mode, dec, iv, ivl, aad, fz_rnd(sizeof aad), param, in, n, out, max + 32, &got);
    if (err == AU_WC_OK && got > max)
        abort();
    /* and back: what was encrypted decrypts to the same */
    if (err == AU_WC_OK && !dec && (mode != AU_WC_GCM || param)) {
        static uint8_t back[2200 + 32];
        size_t bmax = au_wc_aes_out_max(mode, true, got, param), bl = 0;
        if (!bmax || au_wc_aes(k, mode, true, iv, ivl, aad, 0, param, out, got, back, bmax + 32, &bl) != AU_WC_OK) {
            if (mode != AU_WC_GCM)
                abort(); /* GCM was given other additional data */
        } else if (mode != AU_WC_GCM && (bl != n || memcmp(back, in, n))) {
            abort();
        }
    }
    size_t dl = fz_rnd(4) ? fz_rnd(100) : fz_rnd(9000);
    static uint8_t dk[9000];
    au_wc_pbkdf2(any_hash(), k, iv, fz_rnd(sizeof iv), 1 + fz_rnd(3), dk, dl);
    au_wc_hkdf(any_hash(), k, iv, fz_rnd(sizeof iv), aad, fz_rnd(sizeof aad), dk, dl);
    au_wc_key_free(k);
}

static void add_export(AuWcKey *k)
{
    for (int pub = 0; pub < 2; pub++) {
        size_t n = 0;
        uint8_t *e = au_wc_export(k, pub != 0, &n);
        if (e)
            fz_corpus_add(&corpus, e, n);
        au_wc_wipe_free(e, n);
    }
    size_t bl;
    const uint8_t *b = au_wc_key_bytes(k, &bl);
    if (b)
        fz_corpus_add(&corpus, b, bl);
}

static void seeds(void)
{
    static const uint8_t seed[48] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
    if (!au_wc_start(seed, sizeof seed)) {
        fprintf(stderr, "fuzz_webcrypto: the library did not start\n");
        exit(1);
    }
    AuWcKey *k = NULL;
    if (au_wc_generate(AU_WC_RSA, 1024, 65537, AU_WC_RSASSA, AU_DIGEST_SHA256, &k) == AU_WC_OK) {
        add_export(k);
        au_wc_key_free(k);
    }
    static const int curves[] = {256, 384, 521};
    for (int c = 0; c < 3; c++)
        if (au_wc_generate(AU_WC_EC, curves[c], 0, AU_WC_ECDSA, (AuDigestAlg)0, &k) == AU_WC_OK) {
            add_export(k);
            au_wc_key_free(k);
        }
    uint8_t x[32];
    for (int i = 0; i < 32; i++)
        x[i] = (uint8_t)(i * 7 + 1);
    if (au_wc_import(AU_WC_X25519, true, 0, AU_WC_ECDH, (AuDigestAlg)0, x, 32, &k) == AU_WC_OK) {
        add_export(k);
        au_wc_key_free(k);
    }
}

int main(int argc, char **argv)
{
    static const char *const dict[] = {"\x30\x82", "\x30\x81", "\x02\x01\x00", "\x02\x81\x81\x00", "\x02\x82\x01\x01\x00",
                                       "\x04", "\x30\x00", "\x02\x00", "\x02\x01\x01", "\xff\xff\xff\xff"};
    seeds();
    if (argc >= 3 && !strcmp(argv[1], "replay")) {
        for (int i = 2; i < argc; i++) {
            FzBuf b;
            if (fz_read_file(argv[i], &b, 1 << 20)) {
                import_all(b.d, b.n);
                free(b.d);
            }
        }
        printf("fuzz_webcrypto: replayed %d files\n", argc - 2);
        return 0;
    }
    long iters = argc > 1 ? atol(argv[1]) : 1000;
    fz_base_seed = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 1;
    fz_cap(argc > 3 ? atol(argv[3]) : 0);
    const char *first = getenv("AURA_FUZZ_FIRST");
    long it = first ? atol(first) : 0;
    FzBuf b;
    memset(&b, 0, sizeof b);
    for (; it < iters && !fz_expired(); it++) {
        fz_iteration(it);
        const FzBuf *src = &corpus.v[fz_rnd((uint32_t)corpus.n)];
        fz_set(&b, src->d, src->n);
        if (fz_rnd(8))
            fz_mutate(&b, dict, (int)(sizeof dict / sizeof dict[0]), &corpus, 4096);
        fz_save(b.d, b.n);
        import_all(b.d, b.n);
        symmetric();
    }
    printf("fuzz_webcrypto: %ld iterations, %u keys imported, %u operations refused\n", it, imported, failed_ops);
    free(b.d);
    return 0;
}
