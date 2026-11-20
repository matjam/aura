/* Tests for the application services (src/browser/app.c, app_net.c) over
 * an in-memory port: starting with no files, with good files and with a
 * damaged file; when each data set is saved; the Running marker and
 * session restore; the network stores and preferences applied to the
 * network stack. */
#include "browser/app.h"
#include "browser/netlog.h"
#include "browser/script_cache.h"
#include "js/hint_store.h"
#include "net/cookies.h"
#include "net/hsts.h"
#include "net/web_storage.h"
#include "url/url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond)                                                                                               \
    do {                                                                                                          \
        checks++;                                                                                                 \
        if (!(cond)) {                                                                                            \
            failures++;                                                                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
        }                                                                                                         \
    } while (0)

/* ---- The in-memory port ---- */

#define MAX_FILES 64

typedef struct MemFile {
    char key[96]; /* "folder/name", or "/name" for the profile folder */
    AuBuf data;
    int writes;
    bool used;
} MemFile;

typedef struct MemFs {
    MemFile f[MAX_FILES];
    char folders[8][32];
    int nfolders;
    bool fail_writes;
} MemFs;

static MemFs fs;
static uint32_t now_ms = 1000;
static int64_t wall = 1763100000;

static void key_of(const char *folder, const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", folder ? folder : "", name);
}

static MemFile *find(const char *folder, const char *name)
{
    char k[96];
    key_of(folder, name, k, sizeof k);
    for (int i = 0; i < MAX_FILES; i++)
        if (fs.f[i].used && !strcmp(fs.f[i].key, k))
            return &fs.f[i];
    return NULL;
}

static MemFile *make(const char *folder, const char *name)
{
    MemFile *m = find(folder, name);
    if (m)
        return m;
    for (int i = 0; i < MAX_FILES; i++)
        if (!fs.f[i].used) {
            m = &fs.f[i];
            memset(m, 0, sizeof *m);
            m->used = true;
            key_of(folder, name, m->key, sizeof m->key);
            au_buf_init(&m->data);
            return m;
        }
    return NULL;
}

static void put(const char *name, const char *text)
{
    MemFile *m = make(NULL, name);
    au_buf_clear(&m->data);
    au_buf_puts(&m->data, text);
}

static int writes_of(const char *name)
{
    MemFile *m = find(NULL, name);
    return m ? m->writes : 0;
}

static const char *text_of(const char *name)
{
    MemFile *m = find(NULL, name);
    return m ? au_buf_cstr(&m->data) : NULL;
}

static void fs_reset(void)
{
    for (int i = 0; i < MAX_FILES; i++)
        if (fs.f[i].used)
            au_buf_free(&fs.f[i].data);
    memset(&fs, 0, sizeof fs);
}

static void reset_writes(void)
{
    for (int i = 0; i < MAX_FILES; i++)
        fs.f[i].writes = 0;
}

static bool m_read(void *ctx, const char *folder, const char *name, AuBuf *out, size_t max)
{
    (void)ctx;
    MemFile *m = find(folder, name);
    if (!m || m->data.len >= max)
        return false;
    au_buf_append(out, m->data.data ? m->data.data : "", m->data.len);
    return true;
}

static bool m_write(void *ctx, const char *folder, const char *name, const void *data, size_t len)
{
    (void)ctx;
    if (fs.fail_writes)
        return false;
    MemFile *m = make(folder, name);
    if (!m)
        return false;
    au_buf_clear(&m->data);
    au_buf_append(&m->data, data, len);
    m->writes++;
    return true;
}

static bool m_rename(void *ctx, const char *folder, const char *name, const char *new_name)
{
    (void)ctx;
    MemFile *m = find(folder, name);
    if (!m)
        return false;
    MemFile *old = find(folder, new_name);
    if (old) {
        au_buf_free(&old->data);
        old->used = false;
    }
    key_of(folder, new_name, m->key, sizeof m->key);
    return true;
}

static void m_remove(void *ctx, const char *folder, const char *name)
{
    (void)ctx;
    MemFile *m = find(folder, name);
    if (m) {
        au_buf_free(&m->data);
        m->used = false;
    }
}

static bool m_exists(void *ctx, const char *folder, const char *name)
{
    (void)ctx;
    return find(folder, name) != NULL;
}

static bool m_make_folder(void *ctx, const char *folder)
{
    (void)ctx;
    for (int i = 0; i < fs.nfolders; i++)
        if (!strcmp(fs.folders[i], folder))
            return true;
    if (fs.nfolders == 8)
        return false;
    snprintf(fs.folders[fs.nfolders++], sizeof fs.folders[0], "%s", folder);
    return true;
}

static void m_list(void *ctx, const char *folder, void (*each)(void *, const char *, size_t, uint32_t), void *ec)
{
    (void)ctx;
    size_t fl = strlen(folder);
    for (int i = 0; i < MAX_FILES; i++)
        if (fs.f[i].used && !strncmp(fs.f[i].key, folder, fl) && fs.f[i].key[fl] == '/')
            each(ec, fs.f[i].key + fl + 1, fs.f[i].data.len, (uint32_t)i);
}

static uint32_t c_ms(void *ctx) { (void)ctx; return now_ms; }
static int64_t c_wall(void *ctx) { (void)ctx; return wall; }
static int32_t c_offset(void *ctx) { (void)ctx; return -28800; }

static AuAppPort mem_port(void)
{
    AuAppPort p;
    memset(&p, 0, sizeof p);
    p.files.read = m_read;
    p.files.write = m_write;
    p.files.rename = m_rename;
    p.files.remove = m_remove;
    p.files.exists = m_exists;
    p.files.make_folder = m_make_folder;
    p.files.list = m_list;
    p.ms = c_ms;
    p.wall_time = c_wall;
    p.utc_offset = c_offset;
    return p;
}

/* ---- A network that never connects ---- */

static uint32_t n_resolve(void *ctx, const char *host) { (void)ctx, (void)host; return 0; }
static int n_resolve_poll(void *ctx, uint32_t r, AuNetAddr *a) { (void)ctx, (void)r, (void)a; return -1; }
static void n_cancel(void *ctx, uint32_t r) { (void)ctx, (void)r; }
static uint32_t n_connect(void *ctx, const AuNetAddr *a, uint16_t p) { (void)ctx, (void)a, (void)p; return 0; }
static int n_connect_poll(void *ctx, uint32_t s) { (void)ctx, (void)s; return -1; }
static long n_send(void *ctx, uint32_t s, const void *d, size_t n) { (void)ctx, (void)s, (void)d, (void)n; return AU_NETIO_ERROR; }
static long n_recv(void *ctx, uint32_t s, void *b, size_t n) { (void)ctx, (void)s, (void)b, (void)n; return AU_NETIO_ERROR; }
static void n_close(void *ctx, uint32_t s) { (void)ctx, (void)s; }
static uint32_t n_ms(void *ctx) { (void)ctx; return now_ms; }
static int64_t n_wall(void *ctx) { (void)ctx; return wall; }
static bool n_entropy(void *ctx, void *buf, size_t len) { (void)ctx; memset(buf, 7, len); return true; }

static AuNetOps net_ops = {NULL, n_resolve, n_resolve_poll, n_cancel, n_connect, n_connect_poll, n_send, n_recv,
                           n_close, n_ms, n_wall, n_entropy};

static int started_calls, stopping_calls;
static void np_started(void *ctx, AuNet *net, struct AuNetLoader *l) { (void)ctx, (void)net, (void)l; started_calls++; }
static void np_stopping(void *ctx) { (void)ctx; stopping_calls++; }
static void np_status(void *ctx, AuBuf *out) { (void)ctx; au_report_kv(out, "Port", "fine"); }

static AuAppNet mem_net(void)
{
    AuAppNet n;
    memset(&n, 0, sizeof n);
    n.ops = &net_ops;
    n.started = np_started;
    n.stopping = np_stopping;
    n.status = np_status;
    return n;
}

/* ---- A browser ---- */

static AuFontSet *fonts;
static uint32_t b_ms(void *ctx) { (void)ctx; return now_ms; }
static AuTime b_now(void *ctx) { (void)ctx; return (AuTime)wall; }

static AuBrowser *make_browser(void)
{
    AuBrowserHost h;
    memset(&h, 0, sizeof h);
    h.time_ms = b_ms;
    h.now = b_now;
    return au_browser_new(fonts, &h);
}

/* Bookmarks and folders below the root, in order. */
static int count_bookmarks(const AuBookmark *root)
{
    int n = 0;
    const AuBookmark *b = root->first;
    while (b && b != root) {
        n++;
        if (b->first) {
            b = b->first;
            continue;
        }
        while (b != root && !b->next)
            b = b->parent;
        if (b != root)
            b = b->next;
    }
    return n;
}

/* ---- Tests ---- */

static void test_no_files(void)
{
    fs_reset();
    AuBrowser *b = make_browser();
    AuAppPort port = mem_port();
    AuApp *a = au_app_new(b, &port);
    bool unclean = au_app_load(a);
    CHECK(!unclean);
    CHECK(m_exists(NULL, NULL, AU_APP_FILE_RUNNING));
    /* First run: the starting bookmarks, the default preferences. */
    CHECK(count_bookmarks(&b->bookmarks.root) == 5);
    CHECK(b->history.utc_offset == -28800);
    AuSession s;
    CHECK(au_app_session(a, unclean, &s) == AU_APP_RESTORE_NONE);
    CHECK(au_app_start_url(a) == NULL);
    /* The first-run bookmarks count as saved: nothing is written until
     * something changes. */
    reset_writes();
    au_app_save(a, false);
    now_ms += AU_APP_SAVE_DELAY_MS;
    au_app_save(a, false);
    au_app_quit(a);
    CHECK(writes_of(AU_APP_FILE_BOOKMARKS) == 0);
    CHECK(writes_of(AU_APP_FILE_PREFS) == 0);
    /* Running goes at quit. */
    CHECK(!m_exists(NULL, NULL, AU_APP_FILE_RUNNING));
    au_app_free(a);
    au_browser_free(b);
}

static void test_good_files(void)
{
    fs_reset();
    put(AU_APP_FILE_PREFS, "startup=blank\nhistory.days=30\n");
    put(AU_APP_FILE_BOOKMARKS, "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n<DL><p>\n<DT><A HREF=\"http://example.com/\">Ex</A>\n</DL><p>\n");
    AuBrowser *b = make_browser();
    AuAppPort port = mem_port();
    AuApp *a = au_app_new(b, &port);
    CHECK(!au_app_load(a));
    CHECK(b->prefs.startup == AU_STARTUP_BLANK);
    CHECK(b->prefs.history_days == 30);
    CHECK(count_bookmarks(&b->bookmarks.root) == 1);
    CHECK(au_app_start_url(a) && !strcmp(au_app_start_url(a), "about:blank"));
    CHECK(!find(NULL, AU_APP_FILE_PREFS " (damaged)"));
    /* Loading changed nothing to save. */
    reset_writes();
    au_app_quit(a);
    CHECK(writes_of(AU_APP_FILE_PREFS) == 0);
    CHECK(writes_of(AU_APP_FILE_BOOKMARKS) == 0);
    au_app_free(a);
    au_browser_free(b);
}

static void test_damaged_file(void)
{
    fs_reset();
    put(AU_APP_FILE_BOOKMARKS, "this is not a bookmark file at all");
    put(AU_APP_FILE_HISTORY, "\x01\x02 garbage");
    put(AU_APP_FILE_COOKIES, "garbage\tgarbage");
    AuBrowser *b = make_browser();
    AuAppPort port = mem_port();
    AuApp *a = au_app_new(b, &port);
    au_app_load(a);
    /* Renamed, not overwritten, and the defaults used. */
    MemFile *aside = find(NULL, AU_APP_FILE_BOOKMARKS " (damaged)");
    CHECK(aside && !strcmp(au_buf_cstr(&aside->data), "this is not a bookmark file at all"));
    CHECK(!find(NULL, AU_APP_FILE_BOOKMARKS));
    CHECK(find(NULL, AU_APP_FILE_HISTORY " (damaged)") != NULL);
    CHECK(b->history.n == 0);
    AuAppNet n = mem_net();
    CHECK(au_app_net_start(a, &n));
    CHECK(find(NULL, AU_APP_FILE_COOKIES " (damaged)") != NULL);
    CHECK(!find(NULL, AU_APP_FILE_COOKIES));
    CHECK(au_cookie_jar_count(au_app_cookie_jar(a)) == 0);
    /* A second damaged file replaces the first one set aside. */
    put(AU_APP_FILE_HISTORY, "\x01 more garbage");
    au_app_quit(a);
    au_app_net_stop(a);
    au_app_free(a);
    au_browser_free(b);

    b = make_browser();
    a = au_app_new(b, &port);
    au_app_load(a);
    aside = find(NULL, AU_APP_FILE_HISTORY " (damaged)");
    CHECK(aside && !strcmp(au_buf_cstr(&aside->data), "\x01 more garbage"));
    au_app_quit(a);
    au_app_free(a);
    au_browser_free(b);
}

static void test_save_scheduling(void)
{
    fs_reset();
    AuBrowser *b = make_browser();
    AuAppPort port = mem_port();
    AuApp *a = au_app_new(b, &port);
    au_app_load(a);
    reset_writes();
    au_history_visit(&b->history, "http://example.com/", "Example", (AuTime)wall);
    au_app_save(a, false); /* the first unsaved change starts the wait */
    now_ms += AU_APP_SAVE_DELAY_MS - 1;
    au_app_save(a, false);
    CHECK(writes_of(AU_APP_FILE_HISTORY) == 0);
    now_ms += 1;
    au_app_save(a, false);
    CHECK(writes_of(AU_APP_FILE_HISTORY) == 1);
    CHECK(text_of(AU_APP_FILE_HISTORY) && strstr(text_of(AU_APP_FILE_HISTORY), "http://example.com/"));
    /* Only changed sets are written. */
    CHECK(writes_of(AU_APP_FILE_PREFS) == 0);
    CHECK(writes_of(AU_APP_FILE_BOOKMARKS) == 0);
    CHECK(writes_of(AU_APP_FILE_SESSION) == 0);
    now_ms += 10 * AU_APP_SAVE_DELAY_MS;
    au_app_save(a, false);
    CHECK(writes_of(AU_APP_FILE_HISTORY) == 1);
    /* Written at once when forced (quitting). */
    au_bookmark_add(&b->bookmarks, NULL, NULL, "New", "http://new.example/", (AuTime)wall);
    au_app_save(a, false);
    CHECK(writes_of(AU_APP_FILE_BOOKMARKS) == 0);
    au_app_save(a, true);
    CHECK(writes_of(AU_APP_FILE_BOOKMARKS) == 1);
    /* A failed write is tried again after the delay. */
    b->prefs_gen++;
    fs.fail_writes = true;
    au_app_save(a, true);
    fs.fail_writes = false;
    CHECK(writes_of(AU_APP_FILE_PREFS) == 0);
    au_app_save(a, false);
    CHECK(writes_of(AU_APP_FILE_PREFS) == 0);
    now_ms += AU_APP_SAVE_DELAY_MS;
    au_app_save(a, false);
    CHECK(writes_of(AU_APP_FILE_PREFS) == 1);
    /* Scroll positions alone wait half a minute. */
    reset_writes();
    b->scroll_gen++;
    au_app_save(a, false);
    now_ms += AU_APP_SAVE_DELAY_MS;
    au_app_save(a, false);
    CHECK(writes_of(AU_APP_FILE_SESSION) == 0);
    now_ms += AU_APP_SCROLL_SAVE_DELAY_MS;
    au_app_save(a, false);
    CHECK(writes_of(AU_APP_FILE_SESSION) == 1);
    au_app_quit(a);
    au_app_free(a);
    au_browser_free(b);
}

static void test_running_and_session(void)
{
    fs_reset();
    AuBrowser *b = make_browser();
    AuAppPort port = mem_port();
    AuApp *a = au_app_new(b, &port);
    CHECK(!au_app_load(a));
    AuBrowserWindow *w = au_browser_window_new(b, 640, 480);
    au_bw_new_tab(w, "about:blank", true, -1);
    au_app_save(a, true); /* the session is on disk; the run then stops uncleanly */
    CHECK(find(NULL, AU_APP_FILE_SESSION) != NULL);
    CHECK(m_exists(NULL, NULL, AU_APP_FILE_RUNNING));
    au_app_free(a);
    au_browser_free(b);

    /* The next start finds Running: offer to restore. */
    b = make_browser();
    a = au_app_new(b, &port);
    bool unclean = au_app_load(a);
    CHECK(unclean);
    AuSession s;
    CHECK(au_app_session(a, unclean, &s) == AU_APP_RESTORE_ASK);
    CHECK(s.nwindows == 1 && s.windows[0].ntabs == 1);
    au_session_free(&s);
    /* With the preference, restored without asking; a clean start with
     * the default preference restores nothing. */
    b->prefs.startup = AU_STARTUP_RESTORE;
    CHECK(au_app_session(a, false, &s) == AU_APP_RESTORE_YES);
    au_session_free(&s);
    b->prefs.startup = AU_STARTUP_HOME;
    CHECK(au_app_session(a, false, &s) == AU_APP_RESTORE_NONE);
    au_app_quit(a);
    CHECK(!m_exists(NULL, NULL, AU_APP_FILE_RUNNING));
    au_app_free(a);
    au_browser_free(b);

    b = make_browser();
    a = au_app_new(b, &port);
    CHECK(!au_app_load(a));
    au_app_quit(a);
    au_app_free(a);
    au_browser_free(b);
}

static void test_net_stores(void)
{
    fs_reset();
    AuBrowser *b = make_browser();
    AuAppPort port = mem_port();
    AuApp *a = au_app_new(b, &port);
    au_app_load(a);
    AuAppNet n = mem_net();
    started_calls = stopping_calls = 0;
    CHECK(au_app_net_start(a, &n));
    CHECK(started_calls == 1);
    CHECK(au_app_net(a) && au_app_loader(a) && au_app_storage(a));
    CHECK(b->storage == au_app_storage(a));
    /* The folders were made. */
    CHECK(fs.nfolders == 3); /* Cache, Local Storage, Script Cache */

    AuUrl u;
    CHECK(au_url_parse(&u, "https://example.com/", NULL));
    AuCookieRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = &u;
    rq.top_level_nav = true;
    rq.safe_method = true;
    rq.now = wall;
    CHECK(au_cookie_jar_set(au_app_cookie_jar(a), &rq, "keep=1; Max-Age=86400", NULL));
    CHECK(au_cookie_jar_set(au_app_cookie_jar(a), &rq, "session=1", NULL));
    au_url_free(&u);
    reset_writes();
    au_app_save(a, false);
    now_ms += AU_APP_NET_SAVE_DELAY_MS - 1;
    au_app_save(a, false);
    CHECK(writes_of(AU_APP_FILE_COOKIES) == 0);
    now_ms += 1;
    au_app_save(a, false);
    CHECK(writes_of(AU_APP_FILE_COOKIES) == 1);

    char status_text[4096];
    AuBuf st;
    au_buf_init(&st);
    au_app_net_status(a, &st);
    snprintf(status_text, sizeof status_text, "%s", au_buf_cstr(&st));
    au_buf_free(&st);
    CHECK(!strncmp(status_text, "Network\n", 8));
    CHECK(strstr(status_text, "Port") && strstr(status_text, "HTTP cache"));

    au_app_quit(a);
    au_app_net_stop(a);
    CHECK(stopping_calls == 1);
    CHECK(b->storage == NULL);
    au_app_free(a);
    au_browser_free(b);

    /* The persistent cookie comes back; the session cookie does not. */
    b = make_browser();
    a = au_app_new(b, &port);
    au_app_load(a);
    CHECK(au_app_net_start(a, &n));
    AuCookieJar *jar = au_app_cookie_jar(a);
    CHECK(au_cookie_jar_count(jar) == 1);
    CHECK(au_cookie_jar_count(jar) == 1 && !strcmp(au_cookie_jar_at(jar, 0)->name, "keep"));
    /* Clearing cookies at quit leaves an empty file. */
    b->prefs.clear_cookies_on_quit = true;
    au_app_quit(a);
    au_app_net_stop(a);
    au_app_free(a);
    au_browser_free(b);
    b = make_browser();
    a = au_app_new(b, &port);
    au_app_load(a);
    CHECK(au_app_net_start(a, &n));
    CHECK(au_cookie_jar_count(au_app_cookie_jar(a)) == 0);
    au_app_quit(a);
    au_app_free(a); /* stops the network too */
    au_browser_free(b);
}

/* Script hints: read at start, written at most once a minute and at
 * quit, emptied with the cache. */
static void test_script_hints_file(void)
{
    fs_reset();
    put(AU_APP_FILE_SCRIPT_HINTS, "# Aura script hints 1\n3e8 abc a\n");
    AuBrowser *b = make_browser();
    AuAppPort port = mem_port();
    AuApp *a = au_app_new(b, &port);
    au_app_load(a);
    AuAppNet n = mem_net();
    CHECK(au_app_net_start(a, &n));
    const AuJsHintStore *s = b->script_hints;
    CHECK(s != NULL);
    size_t count = 0;
    const uint32_t *o = s ? s->get(s->ctx, 1000, 0xabc, &count) : NULL;
    CHECK(o && count == 1 && o[0] == 10);
    reset_writes();
    if (s)
        s->add(s->ctx, 1000, 0xabc, 20);
    au_app_save(a, false);
    CHECK(writes_of(AU_APP_FILE_SCRIPT_HINTS) == 0);
    now_ms += AU_APP_HINTS_SAVE_MS;
    au_app_save(a, false);
    CHECK(writes_of(AU_APP_FILE_SCRIPT_HINTS) == 1);
    CHECK(!strcmp(text_of(AU_APP_FILE_SCRIPT_HINTS), "# Aura script hints 1\n3e8 abc a a\n"));
    au_app_clear_cache(a);
    au_app_quit(a);
    au_app_net_stop(a);
    CHECK(b->script_hints == NULL);
    CHECK(!strcmp(text_of(AU_APP_FILE_SCRIPT_HINTS), "# Aura script hints 1\n"));
    au_app_free(a);
    au_browser_free(b);
}

/* The script cache: its folder is made with the network stack, a stored
 * script is a file there, the Index is written as rarely as the hints,
 * the cache's size preference bounds it and clearing the cache empties
 * it. */
static void test_script_cache_folder(void)
{
    fs_reset();
    AuBrowser *b = make_browser();
    b->prefs.disk_cache_kb = 1024;
    AuAppPort port = mem_port();
    AuApp *a = au_app_new(b, &port);
    au_app_load(a);
    AuAppNet n = mem_net();
    CHECK(au_app_net_start(a, &n));
    const AuJsCodeStore *s = b->script_code;
    CHECK(s != NULL);
    uint8_t key[AU_JS_CODE_KEY], other[AU_JS_CODE_KEY];
    for (int i = 0; i < AU_JS_CODE_KEY; i++) {
        key[i] = (uint8_t)(i + 1);
        other[i] = (uint8_t)(0xf0 - i);
    }
    static const char name[] = "0102030405060708090a0b0c0d0e0f";
    size_t len = 0;
    bool seen = true;
    if (s) {
        CHECK(!s->get(s->ctx, key, &len, &seen) && !seen);
        CHECK(!s->get(s->ctx, other, &len, &seen) && !seen);
        s->put(s->ctx, key, (const uint8_t *)"bytecode", 8);
        au_app_net_idle(a); /* a port that defers cache writes */
        CHECK(find(AU_APP_FOLDER_SCRIPT_CACHE, name) != NULL);
        uint8_t *code = s->get(s->ctx, key, &len, &seen);
        CHECK(code && len == 8 && !memcmp(code, "bytecode", 8) && seen);
        au_free(code);
        /* the Index (the key asked for once) waits */
        au_app_save(a, false);
        CHECK(!find(AU_APP_FOLDER_SCRIPT_CACHE, AU_SCRIPT_CACHE_INDEX));
        now_ms += AU_APP_HINTS_SAVE_MS;
        au_app_save(a, false);
        CHECK(find(AU_APP_FOLDER_SCRIPT_CACHE, AU_SCRIPT_CACHE_INDEX) != NULL);
        /* with the disk cache off nothing is kept */
        b->prefs.disk_cache_kb = 0;
        au_app_prefs_changed(a);
        CHECK(!find(AU_APP_FOLDER_SCRIPT_CACHE, name));
        s->put(s->ctx, key, (const uint8_t *)"bytecode", 8);
        CHECK(!find(AU_APP_FOLDER_SCRIPT_CACHE, name));
        b->prefs.disk_cache_kb = 1024;
        au_app_prefs_changed(a);
        s->put(s->ctx, key, (const uint8_t *)"bytecode", 8);
        au_app_net_idle(a);
        CHECK(find(AU_APP_FOLDER_SCRIPT_CACHE, name) != NULL);
        au_app_clear_cache(a);
        CHECK(!find(AU_APP_FOLDER_SCRIPT_CACHE, name));
    }
    au_app_quit(a);
    au_app_net_stop(a);
    CHECK(b->script_code == NULL);
    CHECK(!find(AU_APP_FOLDER_SCRIPT_CACHE, AU_SCRIPT_CACHE_INDEX));
    au_app_free(a);
    au_browser_free(b);
}

static void test_prefs_applied(void)
{
    fs_reset();
    put(AU_APP_FILE_PREFS, "privacy.cookies=none\nprivacy.https_first=false\ncookies.sites=example.com=allow\n");
    AuBrowser *b = make_browser();
    AuAppPort port = mem_port();
    AuApp *a = au_app_new(b, &port);
    au_app_load(a);
    AuAppNet n = mem_net();
    CHECK(au_app_net_start(a, &n));
    AuNet *net = au_app_net(a);
    AuCookieJar *jar = au_app_cookie_jar(a);
    bool third = false;
    CHECK(au_net_config(net)->https_first == b->prefs.https_first);
    CHECK(au_cookie_jar_policy(jar, &third) == AU_COOKIE_POLICY_NONE);
    CHECK(au_cookie_jar_rule(jar, "example.com") == AU_COOKIE_RULE_ALLOW);
    /* A change reaches the stack. */
    b->prefs.cookies = AU_COOKIES_SAME_SITE;
    b->prefs.https_first = !b->prefs.https_first;
    b->prefs.send_dnt = true;
    au_app_prefs_changed(a);
    CHECK(au_cookie_jar_policy(jar, &third) == AU_COOKIE_POLICY_SAME_SITE);
    CHECK(au_net_config(net)->https_first == b->prefs.https_first);
    CHECK(au_net_config(net)->send_dnt);
    /* Rules edited in the jar go back to the preference, which is then
     * saved. */
    au_cookie_jar_set_rule(jar, "blocked.example", AU_COOKIE_RULE_BLOCK);
    uint32_t gen = b->prefs_gen;
    au_app_cookie_rules_changed(a);
    CHECK(b->prefs_gen != gen);
    CHECK(b->prefs.cookie_sites && strstr(b->prefs.cookie_sites, "blocked.example=block"));
    reset_writes();
    au_app_quit(a);
    CHECK(writes_of(AU_APP_FILE_PREFS) == 1);
    CHECK(text_of(AU_APP_FILE_PREFS) && strstr(text_of(AU_APP_FILE_PREFS), "blocked.example=block"));
    au_app_free(a);
    au_browser_free(b);
}

/* The stores alone, as aura-host fetch uses them. */
static void test_stores_alone(void)
{
    fs_reset();
    AuAppPort port = mem_port();
    AuHsts *h = au_hsts_new();
    CHECK(au_hsts_process(h, "secure.example", "max-age=31536000", wall));
    au_app_save_stores(&port.files, NULL, h, NULL, wall);
    CHECK(writes_of(AU_APP_FILE_HSTS) == 1);
    au_app_save_stores(&port.files, NULL, h, NULL, wall); /* clean now: not written again */
    CHECK(writes_of(AU_APP_FILE_HSTS) == 1);
    au_hsts_free(h);
    h = au_hsts_new();
    au_app_load_stores(&port.files, NULL, h, NULL, wall);
    CHECK(au_hsts_is_known(h, "secure.example", wall));
    au_hsts_free(h);
}

/* A folder as web storage's file table. */
static void test_folder(void)
{
    fs_reset();
    AuAppPort port = mem_port();
    AuAppFolder f;
    CHECK(au_app_folder_init(&f, &port.files, AU_APP_FOLDER_LOCAL_STORAGE, 100));
    AuWebStorageFiles wf;
    au_app_folder_web_storage(&f, &wf);
    CHECK(wf.write(wf.ctx, "Index", "abc", 3));
    CHECK(find(AU_APP_FOLDER_LOCAL_STORAGE, "Index") != NULL);
    CHECK(f.writes == 1 && f.largest_len == 3);
    AuBuf b;
    au_buf_init(&b);
    CHECK(wf.read(wf.ctx, "Index", &b) && b.len == 3);
    /* An empty file reads as missing, a file too large is not read. */
    CHECK(wf.write(wf.ctx, "Empty", "", 0));
    CHECK(!wf.read(wf.ctx, "Empty", &b));
    char big[200];
    memset(big, 'x', sizeof big);
    CHECK(wf.write(wf.ctx, "Big", big, sizeof big));
    CHECK(!wf.read(wf.ctx, "Big", &b));
    au_buf_free(&b);
    /* A damaged area is set aside. */
    wf.damaged(wf.ctx, "Index");
    CHECK(!find(AU_APP_FOLDER_LOCAL_STORAGE, "Index"));
    CHECK(find(AU_APP_FOLDER_LOCAL_STORAGE, "Index (damaged)") != NULL);
    wf.remove(wf.ctx, "Big");
    CHECK(!find(AU_APP_FOLDER_LOCAL_STORAGE, "Big"));
}

int main(void)
{
    fonts = au_fontset_new();
    test_no_files();
    test_good_files();
    test_damaged_file();
    test_save_scheduling();
    test_running_and_session();
    test_net_stores();
    test_script_hints_file();
    test_script_cache_folder();
    test_prefs_applied();
    test_stores_alone();
    test_folder();
    fs_reset();
    au_fontset_free(fonts);
    printf("app: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
