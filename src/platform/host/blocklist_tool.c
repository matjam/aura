/* aura-host blocklist: compile, check, time and update blocklists
 * (src/net/blocklist.h, blocklist_store.h).
 *
 *   aura-host blocklist compile IN OUT [--title T]
 *       Compiles a text list to the hash file and prints what it cost.
 *   aura-host blocklist check FILE HOST...
 *       Says whether each host is listed. FILE is compiled or text.
 *   aura-host blocklist bench FILE [LOOKUPS]
 *       Times lookups of hosts taken from a text list and of unlisted ones.
 *   aura-host blocklist update DIR [--lists "ID..."] [--url URL]... [--now]
 *       Runs the update manager once against the folder DIR, over the
 *       network, as the browser does at idle. */
#include "host.h"
#include "net_host.h"
#include "net/blocklist.h"
#include "net/blocklist_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* An AuBlockMem that counts, to report the peak. */
typedef struct Counted {
    size_t now, peak;
} Counted;

static void *counted_alloc(void *ctx, size_t n)
{
    Counted *c = ctx;
    size_t *p = malloc(n + 16);
    if (!p)
        return NULL;
    p[0] = n;
    c->now += n;
    if (c->now > c->peak)
        c->peak = c->now;
    return (char *)p + 16;
}

static void counted_release(void *ctx, void *ptr)
{
    Counted *c = ctx;
    size_t *p = (size_t *)((char *)ptr - 16);
    c->now -= p[0];
    free(p);
}

static int usage(void)
{
    fprintf(stderr, "usage: aura-host blocklist compile IN OUT [--title T]\n"
                    "       aura-host blocklist check FILE HOST...\n"
                    "       aura-host blocklist bench LIST.txt [LOOKUPS]\n"
                    "       aura-host blocklist update DIR [--lists \"ID...\"] [--url URL]... [--now]\n");
    return 2;
}

/* Compiles the text file at path. */
static bool compile_file(const char *path, const char *title, const AuBlockMem *mem, uint8_t **data, size_t *len,
                         AuBlockBuildStats *st, double *ms)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "aura-host: cannot read %s\n", path);
        return false;
    }
    double t0 = now_ms();
    AuBlockBuilder *b = au_block_builder_new(mem);
    if (title)
        au_block_builder_set_title(b, title);
    char buf[65536];
    size_t n;
    bool ok = true;
    while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0)
        ok = au_block_builder_feed(b, buf, n);
    fclose(f);
    ok = ok && au_block_builder_finish(b, 0) == 1 && au_block_builder_take(b, data, len);
    if (st)
        au_block_builder_stats(b, st);
    au_block_builder_free(b);
    if (ms)
        *ms = now_ms() - t0;
    if (!ok)
        fprintf(stderr, "aura-host: cannot compile %s\n", path);
    return ok;
}

AuBlocklist *au_host_blocklist_open(const char *path)
{
    size_t len = 0;
    char *raw = au_host_read_file(path, &len);
    if (!raw) {
        fprintf(stderr, "aura-host: cannot read %s\n", path);
        return NULL;
    }
    AuBlocklist *bl = au_blocklist_new();
    uint8_t *data = NULL;
    if (len >= 4 && !memcmp(raw, "AuBL", 4)) {
        data = au_alloc(len);
        memcpy(data, raw, len);
    } else if (!compile_file(path, NULL, NULL, &data, &len, NULL, NULL)) {
        au_free(raw);
        au_blocklist_free(bl);
        return NULL;
    }
    au_free(raw);
    if (!au_blocklist_attach(bl, "file", data, len, NULL)) {
        fprintf(stderr, "aura-host: %s is not a valid compiled blocklist\n", path);
        au_blocklist_free(bl);
        return NULL;
    }
    return bl;
}

void au_host_blocklist_report(AuBlocklist *bl)
{
    AuBlockStats s;
    au_blocklist_stats(bl, &s);
    printf("blocklist: %lu requests checked, %lu blocked, %lu navigations stopped, %lu listed but first-party; "
           "%lu lists, %lu entries, %lu KB\n",
           (unsigned long)s.checked, (unsigned long)s.blocked, (unsigned long)s.navigations,
           (unsigned long)s.first_party, (unsigned long)s.lists, (unsigned long)s.entries,
           (unsigned long)(s.bytes / 1024));
}

static int cmd_compile(int argc, char **argv)
{
    const char *title = NULL;
    if (argc < 5)
        return usage();
    for (int i = 5; i < argc; i++)
        if (!strcmp(argv[i], "--title") && i + 1 < argc)
            title = argv[++i];
        else
            return usage();
    Counted c = {0, 0};
    AuBlockMem mem = {&c, counted_alloc, counted_release};
    uint8_t *data;
    size_t len;
    AuBlockBuildStats st;
    double ms;
    if (!compile_file(argv[3], title, &mem, &data, &len, &st, &ms))
        return 1;
    AuBlockFileInfo info;
    if (!au_block_file_check(data, len, &info)) {
        fprintf(stderr, "aura-host: the compiled list does not check\n");
        return 1;
    }
    bool ok = au_host_write_file(argv[4], data, len);
    struct stat sb;
    long text = stat(argv[3], &sb) == 0 ? (long)sb.st_size : 0;
    printf("%s: \"%s\"\n", argv[4], info.title);
    printf("  %lu lines, %lu names taken (%lu exceptions), %lu lines ignored, %lu too long\n", (unsigned long)st.lines,
           (unsigned long)st.accepted, (unsigned long)st.allowed, (unsigned long)st.ignored,
           (unsigned long)st.too_long);
    printf("  %lu entries and %lu exceptions after removing duplicates\n", (unsigned long)info.blocks,
           (unsigned long)info.allows);
    printf("  text %ld bytes, compiled %zu bytes, compiled in %.1f ms, peak memory %zu KB\n", text, len, ms,
           c.peak / 1024);
    counted_release(&c, data);
    if (!ok) {
        fprintf(stderr, "aura-host: cannot write %s\n", argv[4]);
        return 1;
    }
    return 0;
}

static int cmd_check(int argc, char **argv)
{
    if (argc < 5)
        return usage();
    AuBlocklist *bl = au_host_blocklist_open(argv[3]);
    if (!bl)
        return 1;
    int listed = 0;
    for (int i = 4; i < argc; i++) {
        AuBuf h;
        au_buf_init(&h);
        const char *name = "";
        if (!au_block_clean_domain(argv[i], strlen(argv[i]), &h)) {
            printf("%s: not a domain\n", argv[i]);
        } else if (au_blocklist_listed(bl, au_buf_cstr(&h), &name)) {
            printf("%s: blocked (%s)\n", au_buf_cstr(&h), name);
            listed++;
        } else {
            printf("%s: not listed\n", au_buf_cstr(&h));
        }
        au_buf_free(&h);
    }
    au_blocklist_free(bl);
    return listed ? 0 : 1;
}

static int cmd_bench(int argc, char **argv)
{
    if (argc < 4)
        return usage();
    long want = argc > 4 ? atol(argv[4]) : 1000000;
    size_t len = 0;
    char *raw = au_host_read_file(argv[3], &len);
    if (!raw)
        return 1;
    /* Hosts to ask about: "www." before every 7th name of the list (hits)
     * and made-up names (misses), in equal parts. */
    enum { NHOSTS = 20000 };
    char (*hosts)[80] = au_calloc(NHOSTS * sizeof *hosts);
    int nh = 0, line = 0;
    for (char *p = raw, *end = raw + len; p < end && nh < NHOSTS / 2;) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t l = (size_t)((nl ? nl : end) - p);
        if (l > 3 && l < 70 && p[0] != '#' && p[0] != '!' && !memchr(p, ' ', l) && line++ % 7 == 0) {
            memcpy(hosts[nh], "www.", 4);
            memcpy(hosts[nh] + 4, p, l);
            if (hosts[nh][4 + l - 1] == '\r')
                hosts[nh][4 + l - 1] = 0;
            nh++;
        }
        p = nl ? nl + 1 : end;
    }
    int hits_made = nh;
    for (int i = 0; nh < NHOSTS && i < NHOSTS; i++)
        snprintf(hosts[nh++], 80, "cdn%d.static.unlisted-site-%d.example.com", i % 7, i);
    au_free(raw);
    AuBlocklist *bl = au_host_blocklist_open(argv[3]);
    if (!bl)
        return 1;
    AuBlockStats s0;
    au_blocklist_stats(bl, &s0);
    long hits = 0;
    double t0 = now_ms();
    for (long i = 0; i < want; i++)
        hits += au_blocklist_listed(bl, hosts[(i * 7919) % nh], NULL);
    double ms = now_ms() - t0;
    AuBlockStats s;
    au_blocklist_stats(bl, &s);
    printf("%lu entries: %ld lookups of %d hosts (%d from the list) in %.1f ms, %.0f per second; %ld listed; "
           "%lu answered from the verdict cache\n",
           (unsigned long)s.entries, want, nh, hits_made, ms, want / (ms / 1000.0), hits,
           (unsigned long)s.cache_hits);
    au_blocklist_free(bl);
    au_free(hosts);
    return 0;
}

/* ---- update: a folder as the store ---- */

typedef struct DirStore {
    const char *dir;
} DirStore;

static bool dir_read(void *ctx, const char *name, const AuBlockMem *mem, uint8_t **data, size_t *len)
{
    DirStore *d = ctx;
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", d->dir, name);
    char *raw = au_host_read_file(path, len);
    if (!raw)
        return false;
    if (mem && mem->alloc) {
        uint8_t *p = mem->alloc(mem->ctx, *len ? *len : 1);
        if (p)
            memcpy(p, raw, *len);
        au_free(raw);
        *data = p;
        return p != NULL;
    }
    *data = (uint8_t *)raw;
    return true;
}

static bool dir_write(void *ctx, const char *name, const void *data, size_t len)
{
    DirStore *d = ctx;
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", d->dir, name);
    return au_host_write_file(path, data, len);
}

static void dir_remove(void *ctx, const char *name)
{
    DirStore *d = ctx;
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", d->dir, name);
    remove(path);
}

static int cmd_update(int argc, char **argv)
{
    if (argc < 4)
        return usage();
    DirStore ds = {argv[3]};
    mkdir(ds.dir, 0755);
    AuPrefs prefs;
    au_prefs_init(&prefs);
    bool now = false;
    AuBuf custom;
    au_buf_init(&custom);
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--lists") && i + 1 < argc) {
            au_prefs_set_str(&prefs.block_lists, argv[++i]);
        } else if (!strcmp(argv[i], "--url") && i + 1 < argc) {
            if (custom.len)
                au_buf_putc(&custom, ' ');
            au_buf_puts(&custom, argv[++i]);
        } else if (!strcmp(argv[i], "--now")) {
            now = true;
        } else {
            return usage();
        }
    }
    au_prefs_set_str(&prefs.block_custom, au_buf_cstr(&custom));
    au_buf_free(&custom);
    AuHostSession ses;
    if (!au_host_session_open(&ses, false, false)) {
        fprintf(stderr, "aura-host: cannot set up sockets\n");
        return 3;
    }
    Counted c = {0, 0};
    AuBlockMem mem = {&c, counted_alloc, counted_release};
    AuBlockStore store = {&ds, dir_read, dir_write, dir_remove};
    AuBlocklist *bl = au_blocklist_new();
    AuBlockManager *m = au_block_manager_new(ses.net, bl, &store, &mem);
    au_block_manager_apply_prefs(m, &prefs);
    if (now)
        au_block_manager_update_now(m);
    double t0 = now_ms();
    uint32_t gen = 0;
    /* Until nothing is in progress, then one more pass to save the index. */
    while (now_ms() - t0 < 600000) {
        bool busy = au_block_manager_idle(m, false);
        bool any = au_net_pump(ses.net);
        if (au_block_manager_generation(m) != gen) {
            gen = au_block_manager_generation(m);
            for (size_t i = 0; i < au_block_manager_count(m); i++) {
                AuBlockListInfo li;
                au_block_manager_info(m, i, &li);
                if (li.state == AU_BLOCK_DOWNLOADING || li.state == AU_BLOCK_COMPILING)
                    printf("  %.0f ms: %s %s\n", now_ms() - t0, li.id,
                           li.state == AU_BLOCK_COMPILING ? "compiling" : "downloading");
            }
        }
        if (!busy && !au_net_active(ses.net))
            break;
        if (!any)
            au_host_net_wait(ses.host, 5);
    }
    au_block_manager_idle(m, false);
    for (size_t i = 0; i < au_block_manager_count(m); i++) {
        AuBlockListInfo li;
        au_block_manager_info(m, i, &li);
        if (!li.enabled)
            continue;
        printf("%s (%s): %s, %lu entries, %lu KB, updated %lld, checked %lld%s%s\n", li.id, li.title,
               li.loaded ? "loaded" : "not loaded", (unsigned long)li.entries, (unsigned long)(li.bytes / 1024),
               (long long)li.updated_at, (long long)li.checked_at, li.error[0] ? ", error: " : "", li.error);
    }
    AuBlockCompileStats cs;
    au_block_manager_compile_stats(m, &cs);
    if (cs.id[0])
        printf("last compile: %s, %lu entries from %lu bytes of text, file %lu bytes; download and parse %lu ms, "
               "merge %lu ms in %lu calls\n",
               cs.id, (unsigned long)cs.entries, (unsigned long)cs.text_bytes, (unsigned long)cs.file_bytes,
               (unsigned long)cs.download_ms, (unsigned long)cs.merge_ms, (unsigned long)cs.merge_calls);
    printf("%.0f ms in all, peak list memory %zu KB\n", now_ms() - t0, c.peak / 1024);
    au_block_manager_free(m);
    au_blocklist_free(bl);
    au_host_session_close(&ses);
    au_prefs_free(&prefs);
    return 0;
}

int au_host_blocklist_main(int argc, char **argv)
{
    if (argc < 3)
        return usage();
    if (!strcmp(argv[2], "compile"))
        return cmd_compile(argc, argv);
    if (!strcmp(argv[2], "check"))
        return cmd_check(argc, argv);
    if (!strcmp(argv[2], "bench"))
        return cmd_bench(argc, argv);
    if (!strcmp(argv[2], "update"))
        return cmd_update(argc, argv);
    return usage();
}
