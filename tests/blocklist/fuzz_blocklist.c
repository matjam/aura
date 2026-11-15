/* Fuzzes the blocklist code that reads hostile bytes: the list parser and
 * builder (random and mutated list text, in random pieces), the compiled
 * file check and the lookups on whatever it accepts (mutated compiled
 * files), and the store's index and compiled files as the manager loads
 * them at start.
 *
 *   fuzz_blocklist ITERATIONS SEED [SECONDS]
 *
 * Stops at the iteration count or after SECONDS (default 60), and prints
 * the iterations done. Run it under AddressSanitizer and UBSan. */
#include "net/blocklist.h"
#include "net/blocklist_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t rng_state;

static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static uint32_t below(uint32_t n) { return n ? rnd() % n : 0; }

static const char *const pieces[] = {
    "example", "ads", "tracker", "cdn", "www", "a", "xn--bcher-kva", "co.uk", "com", "net", "github.io", "x-y_z",
    ".", ".", ".", "..", "\n", "\n", "\n", "\r\n", "\r", " ", "\t", "#", "! ", "# Title: ", "||", "@@||", "@@", "^",
    "^$important", "^$third-party", "^|", "*.", "0.0.0.0 ", "127.0.0.1 ", "::1 ", "local=/", "address=/", "/", "/#",
    "[Adblock Plus]", "localhost", "localhost.localdomain", "\xC3\xBC", "\xE3\x80\x82", "\xFF", "%41", "1.2.3.4",
    "-", "_", "0", "9999999999"};
#define NPIECES (sizeof pieces / sizeof pieces[0])

static void random_text(AuBuf *b)
{
    uint32_t n = below(400);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = below(100);
        if (k < 80) {
            au_buf_puts(b, pieces[below(NPIECES)]);
        } else if (k < 90) {
            au_buf_putc(b, (char)below(256));
        } else if (k < 93) {
            /* A long run, to cross the line limit. */
            uint32_t run = below(1500);
            for (uint32_t j = 0; j < run; j++)
                au_buf_putc(b, (char)('a' + below(26)));
        } else {
            au_buf_printf(b, "h%u.d%u.example\n", (unsigned)below(50), (unsigned)below(5));
        }
    }
}

static void lookups(AuBlocklist *bl)
{
    char host[300];
    for (int i = 0; i < 20; i++) {
        size_t n = 0;
        uint32_t parts = 1 + below(6);
        for (uint32_t p = 0; p < parts && n < 250; p++) {
            const char *s = pieces[below(12)];
            size_t l = strlen(s);
            memcpy(host + n, s, l);
            n += l;
            host[n++] = '.';
        }
        host[n ? n - 1 : 0] = 0;
        const char *name;
        au_blocklist_listed(bl, host, &name);
        AuUrl u;
        char url[400], reason[64];
        snprintf(url, sizeof url, "https://%s/x", host);
        if (au_url_parse(&u, url, NULL)) {
            au_blocklist_check(bl, &u, below(2) ? "https://www.example.com/" : NULL, reason, sizeof reason);
            au_url_free(&u);
        }
    }
}

/* Compiles text; returns the compiled file (au_alloc) or NULL. */
static uint8_t *build(const char *text, size_t n, size_t *len)
{
    AuBlockBuilder *b = au_block_builder_new(NULL);
    for (size_t i = 0; i < n;) {
        size_t c = 1 + below(below(4) ? 64 : 2000);
        if (c > n - i)
            c = n - i;
        if (!au_block_builder_feed(b, text + i, c))
            break;
        i += c;
    }
    uint32_t budget = below(3) ? below(50) : 0;
    int r;
    while ((r = au_block_builder_finish(b, budget)) == 0) {
    }
    uint8_t *data = NULL;
    if (r == 1 && au_block_builder_take(b, &data, len)) {
        AuBlockFileInfo info;
        if (!au_block_file_check(data, *len, &info)) {
            fprintf(stderr, "fuzz_blocklist: the builder wrote a file that does not check\n");
            abort();
        }
    }
    au_block_builder_free(b);
    return data;
}

static void mutate(uint8_t *d, size_t len)
{
    uint32_t n = 1 + below(4);
    for (uint32_t i = 0; i < n && len; i++) {
        size_t at = below(3) ? below((uint32_t)(len < 100 ? len : 100)) : below((uint32_t)len);
        switch (below(4)) {
        case 0: d[at] ^= (uint8_t)(1u << below(8)); break;
        case 1: d[at] = (uint8_t)below(256); break;
        case 2: d[at] = 0xFF; break;
        default: d[at] = 0; break;
        }
    }
}

/* A store of two files held in memory. */
typedef struct Mem {
    const uint8_t *index, *list;
    size_t index_len, list_len;
} Mem;

static bool m_read(void *ctx, const char *name, const AuBlockMem *mem, uint8_t **data, size_t *len)
{
    Mem *m = ctx;
    (void)mem;
    const uint8_t *src = !strcmp(name, "Index") ? m->index : m->list;
    size_t n = !strcmp(name, "Index") ? m->index_len : m->list_len;
    if (!src)
        return false;
    *data = au_alloc(n ? n : 1);
    memcpy(*data, src, n);
    *len = n;
    return true;
}

int main(int argc, char **argv)
{
    long iterations = argc > 1 ? atol(argv[1]) : 3000;
    rng_state = argc > 2 ? (uint32_t)atol(argv[2]) : 1;
    long seconds = argc > 3 ? atol(argv[3]) : 60;
    if (!rng_state)
        rng_state = 1;
    time_t start = time(NULL);
    long done = 0, accepted = 0;
    for (; done < iterations; done++) {
        if ((done & 31) == 0 && time(NULL) - start >= seconds)
            break;
        AuBuf text;
        au_buf_init(&text);
        random_text(&text);
        size_t len = 0;
        uint8_t *file = build(text.data ? text.data : "", text.len, &len);
        if (file) {
            /* Mutated copies through the check and, when accepted, lookups. */
            for (int k = 0; k < 8; k++) {
                size_t ml = below(4) ? len : below((uint32_t)len + 8);
                uint8_t *copy = au_alloc(ml + 1);
                memcpy(copy, file, ml < len ? ml : len);
                if (ml > len)
                    memset(copy + len, (int)below(256), ml - len);
                if (k)
                    mutate(copy, ml);
                AuBlocklist *bl = au_blocklist_new();
                if (au_blocklist_attach(bl, "f", copy, ml, NULL)) {
                    accepted++;
                    lookups(bl);
                }
                au_blocklist_free(bl);
            }
            /* The manager loading a store at start: the index is the list
             * text (junk to it) or random lines, the list file a mutated
             * compiled file. */
            uint8_t *copy = au_alloc(len + 1);
            memcpy(copy, file, len);
            if (below(2))
                mutate(copy, len);
            AuBuf index;
            au_buf_init(&index);
            if (below(2)) {
                au_buf_append(&index, text.data, text.len);
            } else {
                for (int i = 0; i < 4; i++)
                    au_buf_printf(&index, "%s\t%u%s\t%u\t%s\t%s\n", below(2) ? "light" : pieces[below(NPIECES)],
                                  (unsigned)rnd(), below(4) ? "" : "x", (unsigned)rnd(), pieces[below(NPIECES)],
                                  pieces[below(NPIECES)]);
            }
            Mem mem = {(const uint8_t *)index.data, copy, index.len, len};
            AuBlockStore store = {&mem, m_read, NULL, NULL};
            AuBlocklist *bl = au_blocklist_new();
            AuBlockManager *mgr = au_block_manager_new(NULL, bl, &store, NULL);
            AuPrefs prefs;
            au_prefs_init(&prefs);
            au_prefs_set_str(&prefs.block_lists, "light normal tif");
            au_prefs_set_str(&prefs.block_custom, below(2) ? "https://lists.example/a.txt http://no.example/b" : "");
            au_block_manager_apply_prefs(mgr, &prefs);
            AuBlockListInfo li;
            for (size_t i = 0; au_block_manager_info(mgr, i, &li); i++) {
            }
            lookups(bl);
            au_prefs_set_str(&prefs.block_lists, "");
            au_prefs_set_str(&prefs.block_custom, "");
            au_block_manager_apply_prefs(mgr, &prefs);
            au_block_manager_free(mgr);
            au_blocklist_free(bl);
            au_prefs_free(&prefs);
            au_buf_free(&index);
            au_free(copy);
            au_free(file);
        }
        au_buf_free(&text);
    }
    printf("fuzz_blocklist: %ld iterations in %ld s, %ld mutated files accepted\n", done, (long)(time(NULL) - start),
           accepted);
    return 0;
}
