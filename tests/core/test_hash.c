/* SHA-256 and MD5 (src/core/hash.c) against published vectors (FIPS 180-4
 * examples, RFC 1321 appendix A.5), fed whole and in pieces of every
 * size, and a million 'a's. */
#include "core/hash.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void hex(const uint8_t *d, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void check_sha(const char *msg, const char *want)
{
    uint8_t d[32];
    char got[65];
    size_t n = strlen(msg);
    au_sha256(msg, n, d);
    hex(d, 32, got);
    if (strcmp(got, want)) {
        printf("FAIL sha256(\"%.20s\"): %s\n", msg, got);
        fails++;
    }
    for (size_t step = 1; step <= n; step++) {
        AuSha256 c;
        au_sha256_init(&c);
        for (size_t i = 0; i < n; i += step)
            au_sha256_update(&c, msg + i, n - i < step ? n - i : step);
        au_sha256_out(&c, d);
        hex(d, 32, got);
        if (strcmp(got, want)) {
            printf("FAIL sha256 in pieces of %lu\n", (unsigned long)step);
            fails++;
            break;
        }
    }
}

static void check_md5(const char *msg, const char *want)
{
    uint8_t d[16];
    char got[33];
    size_t n = strlen(msg);
    for (size_t step = 1; step <= (n ? n : 1); step++) {
        AuMd5 c;
        au_md5_init(&c);
        for (size_t i = 0; i < n; i += step)
            au_md5_update(&c, msg + i, n - i < step ? n - i : step);
        au_md5_out(&c, d);
        hex(d, 16, got);
        if (strcmp(got, want)) {
            printf("FAIL md5(\"%.20s\") in pieces of %lu: %s\n", msg, (unsigned long)step, got);
            fails++;
            break;
        }
    }
}

int main(void)
{
    check_sha("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_sha("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    check_sha("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    {
        AuSha256 c;
        uint8_t d[32], a[1000];
        char got[65];
        memset(a, 'a', sizeof a);
        au_sha256_init(&c);
        for (int i = 0; i < 1000; i++)
            au_sha256_update(&c, a, sizeof a);
        au_sha256_out(&c, d);
        hex(d, 32, got);
        if (strcmp(got, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")) {
            printf("FAIL sha256 of a million 'a'\n");
            fails++;
        }
    }
    check_md5("", "d41d8cd98f00b204e9800998ecf8427e");
    check_md5("a", "0cc175b9c0f1b6a831c399e269772661");
    check_md5("abc", "900150983cd24fb0d6963f7d28e17f72");
    check_md5("message digest", "f96b697d7cb7938d525a2f31aaf161d0");
    check_md5("abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b");
    check_md5("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f");
    check_md5("12345678901234567890123456789012345678901234567890123456789012345678901234567890",
              "57edf4a22be3c955ac49da2e2107b67a");
    if (fails)
        return 1;
    printf("ok\n");
    return 0;
}
