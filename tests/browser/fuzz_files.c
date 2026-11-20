/* Deterministic fuzzer for the files Aura keeps on disk, which can be
 * damaged: Session, Bookmarks.html, History, Aura Preferences, Downloads,
 * the per-site cookie rules, and the disk cache folder (hostile file
 * names, sizes and contents behind AuDiskCacheFiles).
 *
 * Seeds are files the stores themselves write (made at start from a few
 * entries) and the session format's example. Each iteration mutates one
 * and parses it; what parses is serialized and parsed again. The parsers
 * must end, must not read past the data (each input is copied to a heap
 * block of exactly its size) and must not grow memory far beyond the
 * input (counts are checked against the input's size).
 *
 * usage: fuzz_files ITERATIONS SEED SECONDS
 *        fuzz_files replay KIND FILE...   (KIND: session bookmarks history
 *                                          prefs downloads rules diskcache
 *                                          storage hints scripts)
 *
 * storage: the web storage service's area files, its index and the
 * session file's storage lines (run_storage). */
#include "../fuzz.h"
#include "browser/bookmarks.h"
#include "browser/browser.h"
#include "browser/downloads.h"
#include "browser/history.h"
#include "browser/prefs.h"
#include "browser/script_cache.h"
#include "browser/script_hints.h"
#include "js/hint_store.h"
#include "net/cookies.h"
#include "net/disk_cache.h"
#include "net/web_storage.h"

static const char *const dict[] = {
    "\n", "\r", "\r\n", "\t", " ", "=", "#", "# Aura session 1\n", "window ", "window 800 600 0 10 10\n", "tab ", "tab 0\n",
    "tab 4294967295\n", "tab -1\n", "entry\t", "entry\thttp://a.test/\tTitle\t100\n", "http://", "https://a.test/x?y#z",
    "file:///", "about:blank", "javascript:", "data:text/html,", "<DT>", "<DL><p>", "</DL><p>", "<H3>", "</H3>", "<A HREF=\"",
    "\" ADD_DATE=\"", "\">", "</A>", "<!DOCTYPE NETSCAPE-Bookmark-file-1>", "<H1>", "<HR>", "<DD>", "<H3 ADD_DATE=\"1\">f</H3>",
    "true", "false", "on", "off", "\\n", "\\t", "\\\\", "\\", "\"", "allow", "block", "session", "a.test=allow ", "=", ".",
    "home.url=", "fonts.size=", "cache.disk_kb=", "zoom.sites=", "cookies.sites=", "blocking.lists=", "content.javascript=",
    "AURD1\n", "c0123456789abcdef", "http://a.test/\n", "\0", "\xFF", "\xC3\xA9", "\xEF\xBB\xBF", "\xED\xA0\x80",
    "storage\t", "storage\thttp://a.test\tk\tv\n", "%09", "%00", "%25", "%", "%zz", "AuLS", "http://a.test", "^http://b.test",
    "# Aura local storage 1\n", "0000abcd\t40\t2\thttp://a.test\n", "0000abcd", "\t99999999999\t",
    "# Aura script hints 1\n", "ffffffff", "100000000", " 0", " 1", " ffffffff", "3e8 abc a\n",
    "# Aura script cache 1\n", "0123456789abcdef0123456789abcdef\n",
};
#define NDICT ((int)(sizeof dict / sizeof dict[0]))

enum {
    K_SESSION, K_BOOKMARKS, K_HISTORY, K_PREFS, K_DOWNLOADS, K_RULES, K_DISKCACHE, K_STORAGE, K_HINTS, K_SCRIPTS, K_COUNT
};
static const char *const kind_names[K_COUNT] = {"session",   "bookmarks", "history", "prefs", "downloads",
                                                "rules",     "diskcache", "storage", "hints", "scripts"};

static void fail(const char *what)
{
    printf("fuzz_files: %s\n", what);
    abort();
}

/* A heap copy of exactly n bytes (plus nothing): a read past the end is
 * caught by the sanitizer. */
static char *exact(const FzBuf *in)
{
    char *p = (char *)malloc(in->n ? in->n : 1);
    if (in->n)
        memcpy(p, in->d, in->n);
    return p;
}

static void run_session(const FzBuf *in)
{
    char *p = exact(in);
    AuSession s;
    int w, t;
    size_t total = 0;
    memset(&s, 0, sizeof s);
    au_session_parse(&s, p, in->n);
    for (w = 0; w < s.nwindows; w++) {
        if (s.windows[w].ntabs < 0)
            fail("session: negative tab count");
        for (t = 0; t < s.windows[w].ntabs; t++) {
            /* its storage lines go to the storage service, as a restore does */
            const AuSessionTab *st = &s.windows[w].tabs[t];
            AuWebStorage *ws = au_web_storage_new();
            int k;
            if (st->nstorage < 0 || st->nstorage > AU_SESSION_STORAGE_LINES)
                fail("session: storage line count");
            for (k = 0; k < st->nstorage; k++)
                au_web_storage_ns_restore(ws, 1, st->storage[k], strlen(st->storage[k]));
            au_web_storage_free(ws);
            total++;
        }
        total++;
    }
    /* every window and tab takes a line of at least four bytes */
    if (total > in->n / 4 + 1)
        fail("session: more windows and tabs than lines");
    au_session_free(&s);
    free(p);
}

static void run_bookmarks(const FzBuf *in)
{
    char *p = exact(in);
    AuBookmarks bm, bm2;
    AuBuf out;
    const AuBookmark *b;
    au_bookmarks_init(&bm);
    au_bookmarks_parse(&bm, p, in->n);
    if (au_bookmarks_count(&bm) > in->n + 1)
        fail("bookmarks: more entries than bytes");
    for (b = &bm.root; b; b = au_bookmark_next(b, &bm.root, false))
        au_bookmark_depth(b);
    au_bookmarks_complete(&bm, "a");
    au_buf_init(&out);
    au_bookmarks_serialize(&bm, &out);
    au_bookmarks_init(&bm2);
    au_bookmarks_parse(&bm2, out.data ? out.data : "", out.len);
    if (au_bookmarks_count(&bm2) != au_bookmarks_count(&bm))
        fail("bookmarks: the file written does not read back with the same count");
    au_bookmarks_free(&bm2);
    au_buf_free(&out);
    au_bookmarks_free(&bm);
    free(p);
}

static void run_history(const FzBuf *in)
{
    char *p = exact(in);
    AuHistory h, h2;
    AuBuf out;
    AuHistoryGroup g[8];
    au_history_init(&h, 1 + fz_rnd(200));
    au_history_parse(&h, p, in->n);
    if (h.n > h.max_entries)
        fail("history: over the entry limit");
    au_history_groups(&h, g, 8);
    au_history_complete(&h, "a");
    au_history_expire(&h, 1800000000, (int)fz_rnd(3));
    au_buf_init(&out);
    au_history_serialize(&h, &out);
    au_history_init(&h2, 1000);
    if (!au_history_parse(&h2, out.data ? out.data : "", out.len) && h.n)
        fail("history: the file written reads back as damaged");
    if (h2.n != h.n)
        fail("history: the file written does not read back with the same count");
    au_history_free(&h2);
    au_buf_free(&out);
    au_history_free(&h);
    free(p);
}

static void run_prefs(const FzBuf *in)
{
    char *p = exact(in);
    AuPrefs pr, pr2;
    AuBuf out, out2;
    AuCookieJar *jar = au_cookie_jar_new();
    au_prefs_init(&pr);
    au_prefs_parse(&pr, p, in->n);
    if (pr.nextra > in->n / 2 + 1)
        fail("prefs: more unknown keys than lines");
    au_buf_init(&out);
    au_buf_init(&out2);
    au_prefs_serialize(&pr, &out);
    au_prefs_init(&pr2);
    au_prefs_parse(&pr2, out.data ? out.data : "", out.len);
    au_prefs_serialize(&pr2, &out2);
    if (out.len != out2.len || (out.len && memcmp(out.data, out2.data, out.len)))
        fail("prefs: the file written does not read back to itself");
    /* the preferences that are parsed further */
    if (pr.cookie_sites)
        au_cookie_rules_parse(jar, pr.cookie_sites);
    au_prefs_copy(&pr2, &pr);
    au_prefs_free(&pr2);
    au_buf_free(&out);
    au_buf_free(&out2);
    au_prefs_free(&pr);
    au_cookie_jar_free(jar);
    free(p);
}

static void run_downloads(const FzBuf *in)
{
    char *p = exact(in);
    AuDownloads dl;
    AuBuf out;
    au_downloads_init(&dl, NULL);
    au_downloads_parse(&dl, p, in->n);
    au_buf_init(&out);
    au_downloads_serialize(&dl, &out);
    if (out.len > in->n * 4 + 4096)
        fail("downloads: the list grew far beyond the file");
    au_downloads_parse(&dl, out.data ? out.data : "", out.len);
    au_buf_free(&out);
    au_downloads_free(&dl);
    free(p);
}

static void run_rules(const FzBuf *in)
{
    /* NUL-terminated text, as the preference holds it */
    char *p = (char *)malloc(in->n + 1);
    AuCookieJar *jar = au_cookie_jar_new();
    AuBuf out;
    size_t n = 0, i;
    for (i = 0; i < in->n; i++)
        if (in->d[i])
            p[n++] = (char)in->d[i];
    p[n] = 0;
    au_cookie_rules_parse(jar, p);
    au_buf_init(&out);
    au_cookie_rules_serialize(jar, &out);
    if (out.len > n + 16)
        fail("cookie rules: the text written is longer than the text read");
    au_buf_free(&out);
    au_cookie_jar_free(jar);
    free(p);
}

/* ---- the disk cache over a hostile folder ----
 * The input is a list of files: a name line, then a line with the size
 * the listing claims and the real length, then that many bytes. */
typedef struct Folder {
    struct {
        char name[40];
        size_t claimed;
        uint8_t *d;
        size_t n;
        bool gone;
    } f[24];
    int n;
    int reads, writes, removes;
} Folder;

static void f_list(void *ctx, void (*each)(void *, const char *, size_t, uint32_t), void *each_ctx)
{
    Folder *fo = (Folder *)ctx;
    int i;
    for (i = 0; i < fo->n; i++)
        if (!fo->f[i].gone)
            each(each_ctx, fo->f[i].name, fo->f[i].claimed, (uint32_t)(i * 7919u));
}

static bool f_read(void *ctx, const char *name, AuBuf *out)
{
    Folder *fo = (Folder *)ctx;
    int i;
    fo->reads++;
    for (i = 0; i < fo->n; i++)
        if (!fo->f[i].gone && !strcmp(fo->f[i].name, name)) {
            au_buf_append(out, fo->f[i].d, fo->f[i].n);
            return true;
        }
    return false;
}

static bool f_write(void *ctx, const char *name, const void *data, size_t len)
{
    Folder *fo = (Folder *)ctx;
    (void)data;
    fo->writes++;
    if (strlen(name) > 31)
        fail("disk cache: a file name over 31 bytes");
    if (len > (1u << 20))
        fail("disk cache: a file over the limit was written");
    return fo->writes % 5 != 0; /* some writes fail */
}

static void f_remove(void *ctx, const char *name)
{
    Folder *fo = (Folder *)ctx;
    int i;
    fo->removes++;
    for (i = 0; i < fo->n; i++)
        if (!strcmp(fo->f[i].name, name))
            fo->f[i].gone = true;
}

static void run_diskcache(const FzBuf *in)
{
    static const char *const keys[] = {"http://a.test/", "http://b.test/", "http://a.test/x?y", ""};
    Folder fo;
    AuDiskCacheFiles files;
    AuDiskCache *dc;
    AuCacheStorage st;
    const uint8_t *p = in->d, *end = in->d + in->n;
    int i, k;
    memset(&fo, 0, sizeof fo);
    while (p < end && fo.n < 24) {
        const uint8_t *nl = (const uint8_t *)memchr(p, '\n', (size_t)(end - p)), *nl2;
        size_t nlen, claimed = 0, real = 0;
        if (!nl)
            break;
        nlen = (size_t)(nl - p);
        if (nlen > 39)
            nlen = 39;
        memcpy(fo.f[fo.n].name, p, nlen);
        fo.f[fo.n].name[nlen] = 0;
        p = nl + 1;
        nl2 = (const uint8_t *)memchr(p, '\n', (size_t)(end - p));
        if (!nl2)
            break;
        {
            char line[64];
            size_t ll = (size_t)(nl2 - p) < 63 ? (size_t)(nl2 - p) : 63;
            unsigned long a = 0, b = 0;
            memcpy(line, p, ll);
            line[ll] = 0;
            sscanf(line, "%lu %lu", &a, &b);
            claimed = a;
            real = b;
        }
        p = nl2 + 1;
        if (real > (size_t)(end - p))
            real = (size_t)(end - p);
        fo.f[fo.n].claimed = claimed;
        fo.f[fo.n].d = (uint8_t *)malloc(real ? real : 1);
        if (real)
            memcpy(fo.f[fo.n].d, p, real);
        fo.f[fo.n].n = real;
        fo.n++;
        p += real;
    }
    memset(&files, 0, sizeof files);
    files.ctx = &fo;
    files.list = f_list;
    files.read = f_read;
    files.write = f_write;
    files.remove = f_remove;
    dc = au_disk_cache_new(&files, 2000 + fz_rnd(100000));
    au_disk_cache_storage(dc, &st);
    if (fz_rnd(2))
        au_disk_cache_set_deferred(dc, true);
    for (k = 0; k < 12; k++) {
        const char *key = keys[fz_rnd(4)];
        AuBuf out;
        au_buf_init(&out);
        switch (fz_rnd(6)) {
        case 0:
        case 1:
            if (st.load(st.ctx, key, &out) && out.len > in->n + 64)
                fail("disk cache: an entry larger than its file");
            break;
        case 2:
            st.store(st.ctx, key, in->d, in->n < 3000 ? in->n : 3000);
            break;
        case 3:
            st.remove(st.ctx, key);
            break;
        case 4:
            au_disk_cache_idle(dc, 1 + (int)fz_rnd(3));
            break;
        default:
            au_disk_cache_set_max(dc, fz_rnd(8000));
            break;
        }
        au_buf_free(&out);
        au_disk_cache_count(dc);
        au_disk_cache_bytes(dc);
    }
    au_disk_cache_free(dc);
    for (i = 0; i < fo.n; i++)
        free(fo.f[i].d);
}

/* ---- web storage: an area file, the index, a session line ----
 * The input is an area file of http://a.test (half the time with its
 * checksum set right, so that the items behind it are reached), or, when
 * it starts with '#', the folder's index. The service reads it through
 * its files, script's calls run on the area, and what changed is saved
 * (some writes fail). The same bytes are also read as a session file's
 * storage line. */
typedef struct StoreFolder {
    const FzBuf *in;
    bool as_index;
    char *sum_fixed; /* the input with a right checksum, or NULL */
    int damaged, writes;
} StoreFolder;

static uint32_t fnv(const uint8_t *d, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ d[i]) * 16777619u;
    return h;
}

static bool sf_read(void *ctx, const char *name, AuBuf *out)
{
    StoreFolder *sf = (StoreFolder *)ctx;
    if (!strcmp(name, "Index")) {
        if (sf->as_index)
            au_buf_append(out, sf->in->d, sf->in->n);
        else
            au_buf_puts(out, "# Aura local storage 1\n0000abcd\t40\t2\thttp://a.test\n");
        return true;
    }
    if (sf->as_index)
        return false;
    au_buf_append(out, sf->sum_fixed ? (const void *)sf->sum_fixed : (const void *)sf->in->d, sf->in->n);
    return true;
}

static bool sf_write(void *ctx, const char *name, const void *data, size_t len)
{
    StoreFolder *sf = (StoreFolder *)ctx;
    (void)data;
    if (strlen(name) > 8)
        fail("storage: a file name over 8 bytes");
    if (len > (1u << 20))
        fail("storage: a file far larger than what was stored");
    return ++sf->writes % 3 != 0;
}

static void sf_remove(void *ctx, const char *name)
{
    (void)ctx;
    (void)name;
}

static void sf_damaged(void *ctx, const char *name)
{
    (void)name;
    ((StoreFolder *)ctx)->damaged++;
}

static void run_storage(const FzBuf *in)
{
    StoreFolder sf;
    AuWebStorageFiles files = {NULL, sf_read, sf_write, sf_remove, sf_damaged};
    AuWebStorage *ws;
    AuStorageArea *a = NULL;
    AuStorageMap *m;
    AuWebStorageStats st;
    char *p = exact(in);
    memset(&sf, 0, sizeof sf);
    sf.in = in;
    sf.as_index = in->n > 0 && in->d[0] == '#';
    if (!sf.as_index && in->n >= 4 && (fnv(in->d, in->n) & 1)) {
        uint32_t h = fnv(in->d, in->n - 4);
        sf.sum_fixed = (char *)malloc(in->n);
        memcpy(sf.sum_fixed, in->d, in->n);
        sf.sum_fixed[in->n - 4] = (char)(h >> 24);
        sf.sum_fixed[in->n - 3] = (char)(h >> 16);
        sf.sum_fixed[in->n - 2] = (char)(h >> 8);
        sf.sum_fixed[in->n - 1] = (char)h;
    }
    /* the parser alone, on a block of exactly the input's size */
    m = au_storage_map_new();
    if (au_web_storage_parse_file(sf.sum_fixed ? sf.sum_fixed : p, in->n, "http://a.test", m)) {
        if (au_storage_map_count(m) > in->n / 8)
            fail("storage: more items than the file has room for");
    } else if (au_storage_map_count(m)) {
        fail("storage: a refused file left items");
    }
    au_storage_map_free(m);
    /* through the service */
    files.ctx = &sf;
    ws = au_web_storage_new();
    au_web_storage_set_files(ws, &files);
    if (au_web_storage_count(ws) > in->n / 12 + 1)
        fail("storage: more areas than the index has lines");
    if (au_web_storage_open(ws, AU_STORAGE_LOCAL, 0, "http://a.test", NULL, true, &a) == AU_STORAGE_OK && a) {
        const char *k, *v;
        size_t kl, vl;
        uint32_t i, n = au_storage_map_count(au_storage_area_map(a));
        for (i = 0; i < n && i < 64; i++)
            if (au_storage_map_key(au_storage_area_map(a), i, &k, &kl))
                au_storage_map_get(au_storage_area_map(a), k, kl, &v, &vl);
        au_web_storage_set(ws, a, "fuzz", 4, "value", 5, "http://a.test/", NULL);
    }
    au_web_storage_save(ws, 0);
    au_web_storage_stats(ws, &st);
    au_web_storage_release(ws);
    /* as a session file's storage line */
    au_web_storage_ns_restore(ws, 7, p, in->n);
    {
        AuBuf out;
        au_buf_init(&out);
        au_web_storage_ns_serialize(ws, 7, &out);
        if (out.len > in->n * 3 + 64)
            fail("storage: a session line grew");
        au_buf_free(&out);
    }
    au_web_storage_clear(ws);
    au_web_storage_free(ws);
    free(sf.sum_fixed);
    free(p);
}

/* Script Hints: offsets must ascend, lie inside their script and keep to
 * the limits, through a round trip. */
static void run_hints(const FzBuf *in)
{
    char *p = exact(in);
    AuScriptHints *h = au_script_hints_new();
    AuBuf out;
    au_script_hints_parse(h, p, in->n);
    if (au_script_hints_count(h) > AU_SCRIPT_HINTS_SCRIPTS)
        fail("hints: more scripts than the limit");
    au_buf_init(&out);
    au_script_hints_serialize(h, &out);
    if (out.len > in->n + 64)
        fail("hints: the file grew in a round trip");
    AuScriptHints *h2 = au_script_hints_new();
    au_script_hints_parse(h2, out.data ? out.data : "", out.len);
    AuBuf out2;
    au_buf_init(&out2);
    au_script_hints_serialize(h2, &out2);
    if (out.len != out2.len || (out.len && memcmp(out.data, out2.data, out.len)))
        fail("hints: a written file reads back differently");
    au_buf_free(&out2);
    au_buf_free(&out);
    au_script_hints_free(h2);
    au_script_hints_free(h);
    free(p);
}

/* The script cache's Index: at most its limit of keys, each once, and a
 * written file reads back the same. */
static void run_scripts(const FzBuf *in)
{
    char *p = exact(in);
    AuScriptCache *c = au_script_cache_new(NULL, 1 << 20);
    AuScriptCacheStats st;
    AuBuf out;
    au_script_cache_index_parse(c, p, in->n);
    au_script_cache_stats(c, &st);
    if (st.seen > AU_SCRIPT_CACHE_SEEN)
        fail("scripts: more keys than the limit");
    au_buf_init(&out);
    au_script_cache_index_serialize(c, &out);
    if (out.len > in->n + 64)
        fail("scripts: the file grew in a round trip");
    AuScriptCache *c2 = au_script_cache_new(NULL, 1 << 20);
    au_script_cache_index_parse(c2, out.data ? (const char *)out.data : "", out.len);
    AuBuf out2;
    au_buf_init(&out2);
    au_script_cache_index_serialize(c2, &out2);
    if (out.len != out2.len || (out.len && memcmp(out.data, out2.data, out.len)))
        fail("scripts: a written file reads back differently");
    au_buf_free(&out2);
    au_buf_free(&out);
    au_script_cache_free(c2);
    au_script_cache_free(c);
    free(p);
}

static void run_kind(int kind, const FzBuf *in)
{
    switch (kind) {
    case K_HINTS: run_hints(in); break;
    case K_SCRIPTS: run_scripts(in); break;
    case K_STORAGE: run_storage(in); break;
    case K_SESSION: run_session(in); break;
    case K_BOOKMARKS: run_bookmarks(in); break;
    case K_HISTORY: run_history(in); break;
    case K_PREFS: run_prefs(in); break;
    case K_DOWNLOADS: run_downloads(in); break;
    case K_RULES: run_rules(in); break;
    default: run_diskcache(in); break;
    }
}

/* ---- seeds: what the stores write ---- */
static void make_seeds(FzCorpus c[K_COUNT])
{
    AuBuf out;
    au_buf_init(&out);
    fz_corpus_add_str(&c[K_SESSION], "# Aura session 1\nwindow 800 600 1 40 60\ntab 1\nentry\thttp://a.test/\tA page\t0\n"
                                     "entry\thttps://b.test/x?y=1#z\tAnother \xC3\xA9\t1234\ntab 0\nentry\tabout:home\tHome\t0\n"
                                     "window 640 480 0\ntab 0\nentry\tfile:///Disk/page.html\t\n");
    fz_corpus_add_str(&c[K_SESSION], "# Aura session 1\n");
    {
        AuBookmarks bm;
        AuBookmark *f, *g;
        au_bookmarks_init(&bm);
        au_bookmark_add(&bm, NULL, NULL, "Aura & <friends>", "http://a.test/?q=\"1\"&r=2", 1700000000);
        f = au_bookmark_add_folder(&bm, NULL, NULL, "Folder \xC3\xA9", 1700000001);
        au_bookmark_add(&bm, f, NULL, "Inside", "https://b.test/", 1700000002);
        g = au_bookmark_add_folder(&bm, f, NULL, "Deeper", 1700000003);
        au_bookmark_add(&bm, g, NULL, "", "file:///x", 0);
        au_bookmarks_serialize(&bm, &out);
        fz_corpus_add(&c[K_BOOKMARKS], out.data, out.len);
        au_bookmarks_free(&bm);
    }
    {
        AuHistory h;
        au_history_init(&h, 100);
        au_history_visit(&h, "http://a.test/", "A page", 1700000000);
        au_history_visit(&h, "https://b.test/x?y=1#z", "Tabs\tand \xC3\xA9", 1700090000);
        au_history_visit(&h, "http://a.test/", NULL, 1700100000);
        au_history_visit(&h, "about:home", "", 1700200000);
        au_buf_clear(&out);
        au_history_serialize(&h, &out);
        fz_corpus_add(&c[K_HISTORY], out.data, out.len);
        au_history_free(&h);
    }
    {
        AuPrefs p;
        au_prefs_init(&p);
        au_prefs_set_str(&p.zoom_sites, "a.test=120 file=90");
        au_prefs_set_str(&p.cookie_sites, "a.test=allow b.test=block c.test=session");
        au_buf_clear(&out);
        au_prefs_serialize(&p, &out);
        fz_corpus_add(&c[K_PREFS], out.data, out.len);
        au_prefs_free(&p);
    }
    {
        AuDownloads dl;
        au_downloads_init(&dl, NULL);
        au_downloads_add(&dl, "http://a.test/file.zip", "file.zip", "application/zip", 123456, 1700000000);
        au_downloads_add(&dl, "https://b.test/a%20b.sit?x=1", "a b.sit", "", -1, 1700000100);
        au_buf_clear(&out);
        au_downloads_serialize(&dl, &out);
        fz_corpus_add(&c[K_DOWNLOADS], out.data, out.len);
        au_downloads_free(&dl);
    }
    fz_corpus_add_str(&c[K_RULES], "a.test=allow b.test=block c.co.uk=session  d.test=bogus =allow x");
    {
        AuScriptHints *h = au_script_hints_new();
        const AuJsHintStore *s = au_script_hints_store(h);
        for (uint32_t i = 0; i < 40; i++)
            s->add(s->ctx, 70000, 0xdeadbeef, i * 1700 + 3);
        s->add(s->ctx, 5000, 1, 0);
        s->add(s->ctx, 5000, 1, 4999);
        au_buf_clear(&out);
        au_script_hints_serialize(h, &out);
        fz_corpus_add(&c[K_HINTS], out.data, out.len);
        au_script_hints_free(h);
    }
    fz_corpus_add_str(&c[K_SCRIPTS], "# Aura script cache 1\n0123456789abcdef0123456789abcdef\n"
                                     "fedcba9876543210fedcba9876543210\n00000000000000000000000000000000\n");
    fz_corpus_add_str(&c[K_SESSION], "# Aura session 1\nwindow 800 600 0 40 60\ntab 0\nentry\thttp://a.test/\tA page\t0\n"
                                     "storage\thttp://a.test\tkey\tvalue\ta%09b\t%25%00%0A\xED\xA0\x80\n"
                                     "storage\thttp://b.test^http://a.test\tk\t\n");
    {
        /* An area file as the service writes it, the index, a session line. */
        static const char *const items[][2] = {{"key", "value"}, {"", ""}, {"\xED\xA0\x80", "a\tb\nc"}, {"n", "12345"}};
        size_t i;
        uint32_t h;
        au_buf_clear(&out);
        au_buf_append(&out, "AuLS\0\0\0\1\0\0\0\x0d", 12);
        au_buf_puts(&out, "http://a.test");
        au_buf_append(&out, "\0\0\0\4", 4);
        for (i = 0; i < 4; i++) {
            int f;
            for (f = 0; f < 2; f++) {
                size_t n = strlen(items[i][f]);
                char len[4] = {0, 0, 0, (char)n};
                au_buf_append(&out, len, 4);
                au_buf_append(&out, items[i][f], n);
            }
        }
        h = fnv((const uint8_t *)out.data, out.len);
        {
            char sum[4] = {(char)(h >> 24), (char)(h >> 16), (char)(h >> 8), (char)h};
            au_buf_append(&out, sum, 4);
        }
        {
            AuStorageMap *m = au_storage_map_new();
            if (!au_web_storage_parse_file(out.data, out.len, "http://a.test", m) || au_storage_map_count(m) != 4)
                fail("storage: the seed file does not parse");
            au_storage_map_free(m);
        }
        fz_corpus_add(&c[K_STORAGE], out.data, out.len);
        fz_corpus_add_str(&c[K_STORAGE], "# Aura local storage 1\n0000abcd\t40\t2\thttp://a.test\n"
                                         "0000abce\t12\t1\thttps://b.test^https://a.test\n0000abcf\t0\t0\tfile://\n");
        fz_corpus_add_str(&c[K_STORAGE], "http://a.test\tkey\tvalue\ta%09b\t%25%00%0A\xED\xA0\x80");
    }
    {
        char name[19];
        au_disk_cache_name("http://a.test/", name);
        au_buf_clear(&out);
        au_buf_printf(&out, "%s\n%d %d\nAURD1\nhttp://a.test/\nentry blob bytes", name, 37, 37);
        au_buf_printf(&out, "c00000000000000zz\n9 9\nAURD1\nx\nyc0123456789abcdef\n4000000000 0\n");
        au_disk_cache_name("http://b.test/", name);
        au_buf_printf(&out, "%s\n%d %d\nAURD1\nhttp://b.test/\n0123456789", name, 31, 31);
        fz_corpus_add(&c[K_DISKCACHE], out.data, out.len);
    }
    au_buf_free(&out);
}

static int fuzz_main(int argc, char **argv)
{
    long iters = 0, it;
    int first, i, k;
    FzCorpus c[K_COUNT];
    FzBuf in = {0};
    memset(c, 0, sizeof c);

    if (fz_args(argc, argv, &iters, &first)) {
        int kind = -1;
        for (k = 0; k < K_COUNT; k++)
            if (first < argc && !strcmp(argv[first], kind_names[k]))
                kind = k;
        if (kind < 0) {
            printf("usage: fuzz_files replay KIND FILE...\n");
            return 2;
        }
        for (i = first + 1; i < argc; i++) {
            if (!fz_read_file(argv[i], &in, 0))
                continue;
            fz_replay_seed(argv[i]);
            run_kind(kind, &in);
            printf("%s: ok\n", argv[i]);
        }
        return 0;
    }
    make_seeds(c);
    for (it = fz_first; it < iters && !fz_expired(); it++) {
        fz_iteration(it);
        int kind = (int)fz_rnd(K_COUNT);
        const FzBuf *src = &c[kind].v[fz_rnd((uint32_t)c[kind].n)];
        if (fz_rnd(20) == 0) {
            size_t n = fz_rnd(200), j;
            fz_reserve(&in, n);
            for (j = 0; j < n; j++)
                in.d[j] = (uint8_t)fz_next();
            in.n = n;
        } else
            fz_set(&in, src->d, src->n);
        fz_mutate(&in, dict, NDICT, &c[kind], 32 << 10);
        fz_save(in.d, in.n);
        if (getenv("AURA_FUZZ_SAVE") && it % 1 == 0) {
            /* the kind, for replay */
            FILE *f = fopen("fuzz-files-kind.txt", "w");
            if (f) {
                fputs(kind_names[kind], f);
                fclose(f);
            }
        }
        run_kind(kind, &in);
    }
    printf("fuzz_files: %ld iterations\n", it - fz_first);
    return 0;
}

int main(int argc, char **argv) { return fz_main(fuzz_main, argc, argv); }
