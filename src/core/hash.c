/* SHA-256 (FIPS 180-4) and MD5 (RFC 1321). See hash.h. */
#include "hash.h"

#include <string.h>

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

/* ---- SHA-256 ---- */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], s[8];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) |
               p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(s, h, sizeof s);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = s[7] + (ror(s[4], 6) ^ ror(s[4], 11) ^ ror(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) +
                      K256[i] + w[i];
        uint32_t t2 = (ror(s[0], 2) ^ ror(s[0], 13) ^ ror(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        s[7] = s[6];
        s[6] = s[5];
        s[5] = s[4];
        s[4] = s[3] + t1;
        s[3] = s[2];
        s[2] = s[1];
        s[1] = s[0];
        s[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++)
        h[i] += s[i];
}

void au_sha256_init(AuSha256 *c)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
}

void au_sha256_update(AuSha256 *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t have = (size_t)(c->len & 63);
    c->len += len;
    if (have) {
        size_t take = 64 - have < len ? 64 - have : len;
        memcpy(c->buf + have, p, take);
        p += take;
        len -= take;
        if (have + take < 64)
            return;
        sha256_block(c->h, c->buf);
    }
    for (; len >= 64; p += 64, len -= 64)
        sha256_block(c->h, p);
    if (len)
        memcpy(c->buf, p, len);
}

void au_sha256_out(const AuSha256 *c, uint8_t out[32])
{
    uint32_t h[8];
    uint8_t blk[128];
    size_t have = (size_t)(c->len & 63), n = have < 56 ? 64 : 128;
    uint64_t bits = c->len * 8;
    memcpy(h, c->h, sizeof h);
    memset(blk, 0, sizeof blk);
    memcpy(blk, c->buf, have);
    blk[have] = 0x80;
    for (int i = 0; i < 8; i++)
        blk[n - 1 - (size_t)i] = (uint8_t)(bits >> (8 * i));
    sha256_block(h, blk);
    if (n == 128)
        sha256_block(h, blk + 64);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

void au_sha256(const void *data, size_t len, uint8_t out[32])
{
    AuSha256 c;
    au_sha256_init(&c);
    au_sha256_update(&c, data, len);
    au_sha256_out(&c, out);
}

/* ---- MD5 ---- */

static const uint32_t K5[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const uint8_t S5[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                               14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                               4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void md5_block(uint32_t h[4], const uint8_t *p)
{
    uint32_t m[16], a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 16; i++)
        m[i] = p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) | ((uint32_t)p[4 * i + 2] << 16) |
               ((uint32_t)p[4 * i + 3] << 24);
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        f += a + K5[i] + m[g];
        a = d;
        d = c;
        c = b;
        b += rol(f, S5[i]);
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
}

void au_md5_init(AuMd5 *c)
{
    c->h[0] = 0x67452301;
    c->h[1] = 0xefcdab89;
    c->h[2] = 0x98badcfe;
    c->h[3] = 0x10325476;
    c->len = 0;
}

void au_md5_update(AuMd5 *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t have = (size_t)(c->len & 63);
    c->len += len;
    if (have) {
        size_t take = 64 - have < len ? 64 - have : len;
        memcpy(c->buf + have, p, take);
        p += take;
        len -= take;
        if (have + take < 64)
            return;
        md5_block(c->h, c->buf);
    }
    for (; len >= 64; p += 64, len -= 64)
        md5_block(c->h, p);
    if (len)
        memcpy(c->buf, p, len);
}

void au_md5_out(const AuMd5 *c, uint8_t out[16])
{
    uint32_t h[4];
    uint8_t blk[128];
    size_t have = (size_t)(c->len & 63), n = have < 56 ? 64 : 128;
    uint64_t bits = c->len * 8;
    memcpy(h, c->h, sizeof h);
    memset(blk, 0, sizeof blk);
    memcpy(blk, c->buf, have);
    blk[have] = 0x80;
    for (int i = 0; i < 8; i++)
        blk[n - 8 + (size_t)i] = (uint8_t)(bits >> (8 * i));
    md5_block(h, blk);
    if (n == 128)
        md5_block(h, blk + 64);
    for (int i = 0; i < 4; i++) {
        out[4 * i] = (uint8_t)h[i];
        out[4 * i + 1] = (uint8_t)(h[i] >> 8);
        out[4 * i + 2] = (uint8_t)(h[i] >> 16);
        out[4 * i + 3] = (uint8_t)(h[i] >> 24);
    }
}
