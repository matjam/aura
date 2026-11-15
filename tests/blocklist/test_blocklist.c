/* Blocklists (src/net/blocklist.h): the list parser for each format and
 * for junk, the compiled file and its checks, matching by parent domain,
 * the public suffix guard, exceptions and the personal entries, hash
 * collisions, and the request decision (first party, per-site switch,
 * session exception). */
#include "net/blocklist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...)                                                                                          \
    do {                                                                                                          \
        checks++;                                                                                                 \
        if (!(cond)) {                                                                                            \
            failures++;                                                                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                           \
            printf(__VA_ARGS__);                                                                                  \
            printf("\n");                                                                                         \
        }                                                                                                         \
    } while (0)

/* Compiles text, fed in pieces of chunk bytes, into a new set. */
static AuBlocklist *compile(const char *text, size_t chunk, AuBlockBuildStats *st)
{
    AuBlockBuilder *b = au_block_builder_new(NULL);
    size_t n = strlen(text);
    for (size_t i = 0; i < n; i += chunk)
        au_block_builder_feed(b, text + i, n - i < chunk ? n - i : chunk);
    int r = au_block_builder_finish(b, 0);
    uint8_t *data = NULL;
    size_t len = 0;
    AuBlocklist *bl = au_blocklist_new();
    if (r == 1 && au_block_builder_take(b, &data, &len))
        au_blocklist_attach(bl, "t", data, len, NULL);
    if (st)
        au_block_builder_stats(b, st);
    au_block_builder_free(b);
    return bl;
}

static bool listed(AuBlocklist *bl, const char *host)
{
    return au_blocklist_listed(bl, host, NULL);
}

static void test_formats(void)
{
    static const char text[] =
        "# Title: Test List\n"
        "! a comment in the adblock style\n"
        "[Adblock Plus]\n"
        "\n"
        "plain.example\n"
        "  spaced.example  \r\n"
        "UPPER.Example\n"
        "trailing-dot.example.\n"
        "commented.example # why\n"
        "0.0.0.0 hosts.example\n"
        "127.0.0.1\ttab.example other.example # two names\n"
        "::1 localhost\n"
        "0.0.0.0 0.0.0.0\n"
        "127.0.0.1 localhost.localdomain\n"
        "||adblock.example^\n"
        "||important.example^$important\n"
        "||thirdparty.example^$third-party\n"
        "||path.example/ads^\n"
        "@@||good.adblock.example^\n"
        "@@plain-exception.example\n"
        "*.wild.example\n"
        "local=/dnsmasq.example/\n"
        "address=/dnsmasq2.example/#\n"
        "b\xC3\xBC" "cher.example\n"
        "xn--bcher-kva.already.example\n"
        "two words.example\n"
        "example.com##.ad-banner\n"
        "/regex[0-9]+/\n"
        "192.168.1.1\n"
        "nodot\n"
        "bad_char!.example\n"
        "-.example\n"
        "a..b.example\n"
        ".leading.example\n"
        "last-line.example";
    for (size_t chunk = 1; chunk <= 4096; chunk = chunk == 1 ? 7 : chunk * 9) {
        AuBlockBuildStats st;
        AuBlocklist *bl = compile(text, chunk, &st);
        AuBlockFileInfo info;
        CHECK(au_blocklist_has(bl, "t", &info), "compiled (chunk %zu)", chunk);
        CHECK(!strcmp(info.title, "Test List"), "title from the list: %s", info.title);
        static const char *const yes[] = {"plain.example",    "spaced.example",    "upper.example",
                                          "trailing-dot.example", "commented.example", "hosts.example",
                                          "tab.example",      "other.example",     "adblock.example",
                                          "important.example", "wild.example",     "dnsmasq.example",
                                          "dnsmasq2.example", "xn--bcher-kva.example", "xn--bcher-kva.already.example",
                                          "-.example",        "last-line.example"};
        for (size_t i = 0; i < sizeof yes / sizeof yes[0]; i++)
            CHECK(listed(bl, yes[i]), "%s listed (chunk %zu)", yes[i], chunk);
        static const char *const no[] = {"thirdparty.example", "path.example",  "plain-exception.example",
                                         "words.example",      "example.com",   "nodot.example",
                                         "localhost.localdomain", "b.example",  "leading.example",
                                         "why.example",        "names.example", "good.adblock.example"};
        for (size_t i = 0; i < sizeof no / sizeof no[0]; i++)
            CHECK(!listed(bl, no[i]), "%s not listed (chunk %zu)", no[i], chunk);
        CHECK(info.blocks == 17 && info.allows == 1, "%u entries, %u exceptions", (unsigned)info.blocks,
              (unsigned)info.allows);
        CHECK(st.accepted == 18 && st.allowed == 1, "%u taken", (unsigned)st.accepted);
        CHECK(st.ignored == 14, "%u ignored", (unsigned)st.ignored);
        au_blocklist_free(bl);
    }
    /* Long lines are dropped whole, and what follows them still parses. */
    AuBuf b;
    au_buf_init(&b);
    au_buf_puts(&b, "before.example\n");
    for (int i = 0; i < 3000; i++)
        au_buf_puts(&b, "x.");
    au_buf_puts(&b, "long.example\nafter.example\n");
    AuBlockBuildStats st;
    AuBlocklist *bl = compile(au_buf_cstr(&b), 100, &st);
    CHECK(listed(bl, "before.example") && listed(bl, "after.example") && !listed(bl, "long.example"), "long line");
    CHECK(st.too_long == 1, "%u too long", (unsigned)st.too_long);
    au_buf_free(&b);
    au_blocklist_free(bl);
    /* Duplicates are removed, across chunks of the builder too. */
    AuBlockBuilder *bb = au_block_builder_new(NULL);
    char name[64];
    for (int round = 0; round < 2; round++)
        for (int i = 0; i < 40000; i++) {
            snprintf(name, sizeof name, "host%d.dup%d.example", i, i % 97);
            au_block_builder_add(bb, name, strlen(name), false);
        }
    CHECK(au_block_builder_finish(bb, 1000) == 0, "the merge runs in slices");
    int r;
    while ((r = au_block_builder_finish(bb, 1000)) == 0) {
    }
    uint8_t *data = NULL;
    size_t len = 0;
    CHECK(r == 1 && au_block_builder_take(bb, &data, &len), "finished");
    AuBlockFileInfo info;
    CHECK(au_block_file_check(data, len, &info) && info.blocks == 40000, "%u unique of 80000", (unsigned)info.blocks);
    bl = au_blocklist_new();
    CHECK(au_blocklist_attach(bl, "big", data, len, NULL), "attach");
    CHECK(listed(bl, "host0.dup0.example") && listed(bl, "a.host39999.dup35.example") &&
              !listed(bl, "host40000.dup36.example"),
          "members of a large list");
    au_blocklist_free(bl);
    au_block_builder_free(bb);
}

static void test_matching(void)
{
    AuBlocklist *bl = compile("ads.example.com\ntracker.net\nsub.deep.example.org\nco.uk\ncom\n"
                              "evil.co.uk\nuser.github.io\ngithub.io\n@@||ok.tracker.net^\n",
                              64, NULL);
    /* A host is listed through itself or any parent down to the
     * registrable domain. */
    CHECK(listed(bl, "ads.example.com") && listed(bl, "x.ads.example.com") && listed(bl, "a.b.c.ads.example.com"),
          "subdomains of a listed name");
    CHECK(!listed(bl, "example.com") && !listed(bl, "www.example.com") && !listed(bl, "notads.example.com"),
          "siblings and parents are not");
    CHECK(listed(bl, "tracker.net") && listed(bl, "cdn.tracker.net"), "registrable domain listed");
    CHECK(!listed(bl, "deep.example.org") && listed(bl, "sub.deep.example.org") && listed(bl, "x.sub.deep.example.org"),
          "deep entry");
    /* Public suffixes: a list naming one blocks nothing through it. */
    CHECK(!listed(bl, "innocent.co.uk") && !listed(bl, "www.innocent.co.uk") && !listed(bl, "co.uk"),
          "co.uk in the list does not block its registrants");
    CHECK(!listed(bl, "anything.com") && !listed(bl, "com"), "nor does com");
    CHECK(listed(bl, "evil.co.uk") && listed(bl, "www.evil.co.uk"), "a registrant of co.uk");
    CHECK(listed(bl, "user.github.io") && listed(bl, "x.user.github.io") && !listed(bl, "other.github.io"),
          "private suffixes: one user's pages, not everyone's");
    /* Hosts without a registrable domain. */
    CHECK(!listed(bl, "localhost") && !listed(bl, "10.0.0.1") && !listed(bl, ""), "no domain");
    /* A list's exception covers the name and what is under it. */
    CHECK(!listed(bl, "ok.tracker.net") && !listed(bl, "a.ok.tracker.net") && listed(bl, "no.tracker.net"),
          "list exception");
    /* Very deep names still reach the registrable domain. */
    char deep[254] = "";
    for (int i = 0; i < 40; i++)
        strcat(deep, "a.");
    strcat(deep, "tracker.net");
    CHECK(listed(bl, deep), "40 labels above a listed domain");

    /* The personal entries: allow beats everything, block is a list. */
    au_blocklist_set_user(bl, "cdn.tracker.net Example.ORG", "mine.example, www.other.example", NULL);
    CHECK(!listed(bl, "cdn.tracker.net") && !listed(bl, "x.cdn.tracker.net") && listed(bl, "tracker.net"),
          "personal allow covers the name and below");
    CHECK(!listed(bl, "sub.deep.example.org"), "personal allow of a parent domain");
    const char *name = NULL;
    CHECK(au_blocklist_listed(bl, "a.mine.example", &name) && !strcmp(name, "Personal blocklist"), "personal block");
    CHECK(listed(bl, "www.other.example") && !listed(bl, "other.example"), "personal block is exact or below");
    au_blocklist_set_user(bl, "mine.example", "mine.example", NULL);
    CHECK(!listed(bl, "mine.example"), "allow beats the personal block");
    au_blocklist_set_user(bl, NULL, NULL, NULL);
    CHECK(listed(bl, "cdn.tracker.net"), "the verdicts are forgotten when the entries change");
    CHECK(au_blocklist_listed(bl, "cdn.tracker.net", &name) && !strcmp(name, "t"), "the list's name: %s", name);

    /* Two lists: the first that names the host gives the reason; an
     * exception in either clears both. */
    AuBlockBuilder *b = au_block_builder_new(NULL);
    au_block_builder_set_title(b, "Second");
    au_block_builder_feed(b, "second.example\ntracker.net\n@@||x.ads.example.com^\n", 52);
    uint8_t *data = NULL;
    size_t len = 0;
    CHECK(au_block_builder_finish(b, 0) == 1 && au_block_builder_take(b, &data, &len), "second list");
    au_block_builder_free(b);
    CHECK(au_blocklist_attach(bl, "second", data, len, NULL), "attach second");
    CHECK(au_blocklist_listed(bl, "second.example", &name) && !strcmp(name, "Second"), "second list's name");
    CHECK(au_blocklist_listed(bl, "tracker.net", &name) && !strcmp(name, "t"), "first list wins the name");
    CHECK(!listed(bl, "x.ads.example.com") && listed(bl, "y.ads.example.com"), "exception across lists");
    au_blocklist_detach(bl, "t");
    CHECK(!listed(bl, "ads.example.com") && listed(bl, "tracker.net"), "after detaching the first");
    AuBlockStats st;
    au_blocklist_stats(bl, &st);
    CHECK(st.lists == 1 && st.entries == 2, "stats");
    au_blocklist_free(bl);
}

/* Two names with one hash cannot be told apart; the design accepts that
 * (a 48-bit hash makes it rare) and gives the user the allow entry, kept
 * as text, to clear it. A collision is made here by putting an innocent
 * name's hash in a list directly. */
static void test_collisions(void)
{
    AuBlockBuilder *b = au_block_builder_new(NULL);
    au_block_builder_add(b, "listed.example", 14, false);
    uint64_t h = au_block_hash("innocent.example", 16);
    CHECK(h < ((uint64_t)1 << 48), "48 bits");
    CHECK(h != au_block_hash("innocent.exampl", 15) && h != au_block_hash("innocenu.example", 16), "hash varies");
    CHECK(au_block_builder_add_hash(b, h, false), "add hash");
    CHECK(au_block_builder_add_hash(b, h, false), "the same hash again");
    uint8_t *data = NULL;
    size_t len = 0;
    CHECK(au_block_builder_finish(b, 0) == 1 && au_block_builder_take(b, &data, &len), "finish");
    au_block_builder_free(b);
    AuBlockFileInfo info;
    CHECK(au_block_file_check(data, len, &info) && info.blocks == 2, "equal hashes are stored once");
    AuBlocklist *bl = au_blocklist_new();
    au_blocklist_attach(bl, "c", data, len, NULL);
    CHECK(listed(bl, "innocent.example") && listed(bl, "www.innocent.example"), "the colliding name is blocked");
    au_blocklist_set_user(bl, "innocent.example", NULL, NULL);
    CHECK(!listed(bl, "innocent.example") && !listed(bl, "www.innocent.example") && listed(bl, "listed.example"),
          "the allow entry clears it");
    au_blocklist_free(bl);
    /* No two of 200 thousand generated names collide. */
    b = au_block_builder_new(NULL);
    char name[64];
    for (int i = 0; i < 200000; i++) {
        snprintf(name, sizeof name, "n%d.c%d.example", i, i / 1000);
        au_block_builder_add(b, name, strlen(name), false);
    }
    CHECK(au_block_builder_finish(b, 0) == 1 && au_block_builder_take(b, &data, &len), "finish");
    CHECK(au_block_file_check(data, len, &info) && info.blocks == 200000, "%u distinct hashes of 200000",
          (unsigned)info.blocks);
    au_free(data);
    au_block_builder_free(b);
}

static void test_files(void)
{
    AuBlockBuilder *b = au_block_builder_new(NULL);
    au_block_builder_set_title(b, "File Test");
    for (int i = 0; i < 300; i++) {
        char name[64];
        snprintf(name, sizeof name, "h%d.example", i);
        au_block_builder_add(b, name, strlen(name), i % 50 == 0);
    }
    uint8_t *data = NULL;
    size_t len = 0;
    CHECK(au_block_builder_finish(b, 0) == 1 && au_block_builder_take(b, &data, &len), "finish");
    au_block_builder_free(b);
    AuBlockFileInfo info;
    CHECK(au_block_file_check(data, len, &info) && info.blocks == 294 && info.allows == 6, "valid");
    CHECK(len == AU_BLOCK_HEADER + 300 * AU_BLOCK_HASH_BYTES && !strcmp(info.title, "File Test"), "layout");
    /* Truncated at every length, and every single byte changed. */
    int accepted = 0;
    for (size_t n = 0; n < len; n++)
        accepted += au_block_file_check(data, n, NULL);
    CHECK(accepted == 0, "%d truncations accepted", accepted);
    uint8_t *copy = au_alloc(len + 6);
    for (size_t i = 0; i < len; i++) {
        memcpy(copy, data, len);
        copy[i] ^= 0x40;
        /* The reserved word and the title are not covered by the checksum;
         * changes there do not make the entries wrong. */
        bool free_byte = i >= 20 && i < AU_BLOCK_HEADER - 1;
        if (!free_byte)
            accepted += au_block_file_check(copy, len, NULL);
    }
    CHECK(accepted == 0, "%d corruptions accepted", accepted);
    memcpy(copy, data, len);
    memset(copy + len, 0, 6);
    CHECK(!au_block_file_check(copy, len + 6, NULL), "trailing bytes refused");
    /* Unsorted entries with a matching checksum are refused too. */
    memcpy(copy, data, len);
    uint8_t tmp[6];
    memcpy(tmp, copy + AU_BLOCK_HEADER, 6);
    memcpy(copy + AU_BLOCK_HEADER, copy + AU_BLOCK_HEADER + 6, 6);
    memcpy(copy + AU_BLOCK_HEADER + 6, tmp, 6);
    uint32_t hsh = 2166136261u;
    for (size_t i = AU_BLOCK_HEADER; i < len; i++) {
        hsh ^= copy[i];
        hsh *= 16777619u;
    }
    copy[16] = (uint8_t)(hsh >> 24);
    copy[17] = (uint8_t)(hsh >> 16);
    copy[18] = (uint8_t)(hsh >> 8);
    copy[19] = (uint8_t)hsh;
    CHECK(!au_block_file_check(copy, len, NULL), "unsorted entries refused");
    /* A damaged file is not attached, and the list in use stays. */
    AuBlocklist *bl = au_blocklist_new();
    CHECK(au_blocklist_attach(bl, "f", data, len, NULL), "attach the good file");
    memcpy(copy, data, len);
    copy[AU_BLOCK_HEADER + 9] ^= 1;
    uint8_t *bad = au_alloc(len);
    memcpy(bad, copy, len);
    CHECK(!au_blocklist_attach(bl, "f", bad, len, NULL), "a damaged file is refused");
    CHECK(listed(bl, "h7.example") && !listed(bl, "h50.example"), "the old list is still in use");
    CHECK(!au_blocklist_attach(bl, "g", NULL, 0, NULL), "nothing at all");
    au_free(copy);
    au_blocklist_free(bl);
    /* An empty list compiles to a valid empty file. */
    bl = compile("# nothing here\n", 16, NULL);
    CHECK(au_blocklist_has(bl, "t", &info) && info.blocks == 0, "empty list");
    CHECK(!listed(bl, "anything.example"), "which blocks nothing");
    au_blocklist_free(bl);
}

static void *no_memory(void *ctx, size_t n)
{
    int *left = ctx;
    if (*left <= 0)
        return NULL;
    (*left)--;
    return malloc(n);
}

static void some_release(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

static void test_out_of_memory(void)
{
    for (int budget = 0; budget < 6; budget++) {
        int left = budget;
        AuBlockMem mem = {&left, no_memory, some_release};
        AuBlockBuilder *b = au_block_builder_new(&mem);
        bool ok = true;
        char name[64];
        for (int i = 0; i < 40000 && ok; i++) {
            snprintf(name, sizeof name, "m%d.example", i);
            ok = au_block_builder_feed(b, name, strlen(name)) && au_block_builder_feed(b, "\n", 1);
        }
        int r = ok ? au_block_builder_finish(b, 0) : -1;
        uint8_t *data = NULL;
        size_t len = 0;
        /* 40000 names need three chunks and the output block. */
        CHECK((r == 1) == (budget >= 4), "budget %d: finish %d", budget, r);
        if (r == 1 && au_block_builder_take(b, &data, &len)) {
            CHECK(au_block_file_check(data, len, NULL), "valid with enough memory");
            free(data);
        }
        au_block_builder_free(b);
    }
}

static bool check(AuBlocklist *bl, const char *url, const char *top, char *reason)
{
    AuUrl u;
    char r[96];
    if (!au_url_parse(&u, url, NULL))
        return false;
    bool blocked = au_blocklist_check(bl, &u, top, r, sizeof r);
    if (reason)
        strcpy(reason, r);
    au_url_free(&u);
    return blocked;
}

static void test_decisions(void)
{
    AuBlocklist *bl = compile("# Title: Ads\nads.net\ntrack.news.example\nlisted-site.com\n", 64, NULL);
    char reason[96];
    const char *page = "https://www.news.example/story";
    /* Third-party subresources and script requests. */
    CHECK(check(bl, "https://cdn.ads.net/a.js", page, reason) && !strcmp(reason, "blocked: Ads"), "third party: %s",
          reason);
    CHECK(!check(bl, "https://cdn.fine.net/a.js", page, reason) && !reason[0], "unlisted third party");
    /* First party: the page's own registrable domain, listed or not. */
    CHECK(!check(bl, "https://track.news.example/p.gif", page, NULL), "first-party listed host is let through");
    CHECK(!check(bl, "https://listed-site.com/app.js", "https://www.listed-site.com/", NULL),
          "a listed site's own resources");
    CHECK(check(bl, "https://track.news.example/p.gif", "https://other.example/", NULL),
          "the same host from another site");
    /* Navigations (no top-level URL). */
    CHECK(check(bl, "https://listed-site.com/", NULL, reason) && !strcmp(reason, "blocked: Ads"), "navigation");
    CHECK(!check(bl, "https://www.news.example/", NULL, NULL), "navigation to an unlisted site");
    /* Addresses and other hosts are never blocked. */
    CHECK(!check(bl, "http://10.1.2.3/x", page, NULL) && !check(bl, "http://localhost/x", page, NULL), "no domain");
    /* The session exception covers navigations to that site only. */
    au_blocklist_allow_session(bl, "WWW.listed-site.com");
    CHECK(!check(bl, "https://listed-site.com/", NULL, NULL) && !check(bl, "https://m.listed-site.com/x", NULL, NULL),
          "Visit Anyway covers the site");
    CHECK(check(bl, "https://listed-site.com/w.js", page, NULL), "but not its use by other sites");
    CHECK(check(bl, "https://ads.net/", NULL, NULL), "nor other listed sites");
    /* The per-site switch. */
    au_blocklist_set_user(bl, NULL, NULL, "news.example");
    CHECK(!check(bl, "https://cdn.ads.net/a.js", page, NULL), "blocking off for the page's site");
    CHECK(check(bl, "https://cdn.ads.net/a.js", "https://elsewhere.example/", NULL), "on elsewhere");
    CHECK(au_blocklist_site_off(bl, page) && !au_blocklist_site_off(bl, "https://elsewhere.example/"), "site_off");
    au_blocklist_set_user(bl, NULL, NULL, "ads.net");
    CHECK(!check(bl, "https://ads.net/", NULL, NULL), "a switched-off site can be visited");
    au_blocklist_set_user(bl, NULL, NULL, NULL);
    /* Off as a whole. */
    au_blocklist_set_enabled(bl, false);
    CHECK(!check(bl, "https://cdn.ads.net/a.js", page, NULL), "blocking off");
    au_blocklist_set_enabled(bl, true);
    AuBlockStats st;
    au_blocklist_stats(bl, &st);
    CHECK(st.blocked == 4 && st.navigations == 2 && st.first_party == 2, "counts: %u blocked, %u navigations, %u first",
          (unsigned)st.blocked, (unsigned)st.navigations, (unsigned)st.first_party);
    CHECK(st.cache_hits > 0, "verdict cache used");
    au_blocklist_free(bl);

    /* The personal blocklist works without any list. */
    bl = au_blocklist_new();
    CHECK(!check(bl, "https://x.mine.example/", NULL, NULL), "nothing listed");
    au_blocklist_set_user(bl, NULL, "mine.example", NULL);
    CHECK(check(bl, "https://x.mine.example/", NULL, reason) && !strcmp(reason, "blocked: Personal blocklist"),
          "personal entry alone: %s", reason);
    au_blocklist_free(bl);
}

static void test_words(void)
{
    CHECK(au_block_words_has("a.example b.example", "b.example") && !au_block_words_has("a.example", "example"),
          "words_has");
    CHECK(!au_block_words_has(NULL, "a") && !au_block_words_has("a", ""), "empty");
    AuBuf b;
    au_buf_init(&b);
    au_block_words_set("a.example  b.example", "c.example", true, &b);
    CHECK(!strcmp(au_buf_cstr(&b), "a.example b.example c.example"), "add: %s", au_buf_cstr(&b));
    au_buf_clear(&b);
    au_block_words_set("a.example b.example c.example", "b.example", false, &b);
    CHECK(!strcmp(au_buf_cstr(&b), "a.example c.example"), "remove: %s", au_buf_cstr(&b));
    au_buf_clear(&b);
    au_block_words_set("a.example", "a.example", true, &b);
    CHECK(!strcmp(au_buf_cstr(&b), "a.example"), "no duplicate: %s", au_buf_cstr(&b));
    static const struct {
        const char *in, *out;
    } clean[] = {{"  Example.COM ", "example.com"},
                 {"https://www.Example.com/path?q#f", "www.example.com"},
                 {"example.com:8080", "example.com"},
                 {"*.example.com", "example.com"},
                 {"b\xC3\xBC" "cher.de", "xn--bcher-kva.de"},
                 {"not a domain", NULL},
                 {"nodot", NULL},
                 {"1.2.3.4", NULL},
                 {"", NULL}};
    for (size_t i = 0; i < sizeof clean / sizeof clean[0]; i++) {
        au_buf_clear(&b);
        bool ok = au_block_clean_domain(clean[i].in, strlen(clean[i].in), &b);
        CHECK(ok == (clean[i].out != NULL) && (!ok || !strcmp(au_buf_cstr(&b), clean[i].out)), "clean \"%s\": %s",
              clean[i].in, ok ? au_buf_cstr(&b) : "(refused)");
    }
    au_buf_clear(&b);
    CHECK(au_block_site_of("https://a.b.example.co.uk:8443/x", &b) && !strcmp(au_buf_cstr(&b), "example.co.uk"),
          "site: %s", au_buf_cstr(&b));
    au_buf_clear(&b);
    CHECK(au_block_site_of("http://127.0.0.1/", &b) && !strcmp(au_buf_cstr(&b), "127.0.0.1"), "address site");
    au_buf_clear(&b);
    CHECK(!au_block_site_of("about:blank", &b) && !au_block_site_of("not a url", &b), "no host");
    au_buf_free(&b);
}

int main(void)
{
    test_formats();
    test_matching();
    test_collisions();
    test_files();
    test_out_of_memory();
    test_decisions();
    test_words();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
