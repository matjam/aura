/* The browser model and controller. See browser.h. */
#include "browser.h"
#include "url.h"

#include <stdio.h>
#include <string.h>

/* ---- Back/forward list ---- */

void au_nav_init(AuNavList *l)
{
    memset(l, 0, sizeof *l);
    l->index = -1;
}

static void free_entry(AuNavEntry *e)
{
    au_free(e->url);
    au_free(e->title);
}

void au_nav_free(AuNavList *l)
{
    for (int i = 0; i < l->n; i++)
        free_entry(&l->e[i]);
    au_free(l->e);
    au_nav_init(l);
}

void au_nav_push(AuNavList *l, const char *url, const char *title)
{
    for (int i = l->index + 1; i < l->n; i++)
        free_entry(&l->e[i]);
    l->n = l->index + 1;
    if (l->n == AU_NAV_MAX) {
        free_entry(&l->e[0]);
        memmove(&l->e[0], &l->e[1], (size_t)(l->n - 1) * sizeof *l->e);
        l->n--;
    }
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->e = au_realloc(l->e, (size_t)l->cap * sizeof *l->e);
    }
    AuNavEntry *e = &l->e[l->n++];
    e->url = au_strndup(url, strlen(url));
    e->title = au_strndup(title ? title : "", title ? strlen(title) : 0);
    e->scroll_y = 0;
    l->index = l->n - 1;
}

AuNavEntry *au_nav_current(AuNavList *l)
{
    return l->index >= 0 && l->index < l->n ? &l->e[l->index] : NULL;
}

bool au_nav_can_go(const AuNavList *l, int delta)
{
    int i = l->index + delta;
    return l->index >= 0 && i >= 0 && i < l->n;
}

AuNavEntry *au_nav_go(AuNavList *l, int delta)
{
    if (!au_nav_can_go(l, delta))
        return NULL;
    l->index += delta;
    return &l->e[l->index];
}

static void set_str(char **f, const char *s)
{
    char *v = au_strndup(s ? s : "", s ? strlen(s) : 0);
    au_free(*f);
    *f = v;
}

/* ---- Host helpers ---- */

static void invalidate(AuBrowserWindow *w, AuIRect r)
{
    AuBrowserHost *h = &w->browser->host;
    r = au_irect_intersect(r, (AuIRect){0, 0, w->width, w->height});
    if (!au_irect_empty(r) && h->invalidate)
        h->invalidate(h->ctx, w, r);
}

static void invalidate_all(AuBrowserWindow *w)
{
    invalidate(w, (AuIRect){0, 0, w->width, w->height});
}

static void changed(AuBrowserWindow *w, unsigned what)
{
    AuBrowserHost *h = &w->browser->host;
    if (h->changed)
        h->changed(h->ctx, w, what);
}

static AuTime now(AuBrowser *b)
{
    return b->host.now ? b->host.now(b->host.ctx) : 0;
}

static uint32_t time_ms(AuBrowser *b)
{
    return b->host.time_ms ? b->host.time_ms(b->host.ctx) : 0;
}

static int tab_index(AuBrowserWindow *w, AuTab *t)
{
    for (int i = 0; i < w->ntabs; i++)
        if (w->tabs[i] == t)
            return i;
    return -1;
}

static bool is_active(AuTab *t)
{
    AuBrowserWindow *w = t->win;
    return w->active >= 0 && w->active < w->ntabs && w->tabs[w->active] == t;
}

static void invalidate_tab(AuBrowserWindow *w, int i)
{
    if (i < 0 || i >= w->ntabs)
        return;
    AuIRect r = au_chrome_tab_rect(&w->layout, i);
    /* The active top tab overlaps its neighbours by a pixel. */
    r.x0 -= 2;
    r.x1 += 2;
    invalidate(w, au_irect_intersect(r, w->layout.tabstrip));
}

AuIRect au_bw_hit_rect(AuBrowserWindow *w, AuHit h)
{
    const AuChromeLayout *l = &w->layout;
    switch (h.part) {
    case AU_HIT_TAB:
    case AU_HIT_TAB_CLOSE: {
        AuIRect r = au_chrome_tab_rect(l, h.index);
        r.x0 -= 2;
        r.x1 += 2;
        return au_irect_intersect(r, l->tabstrip);
    }
    case AU_HIT_NEW_TAB: return l->new_tab;
    case AU_HIT_TAB_SCROLL_BACK: return l->scroll_back;
    case AU_HIT_TAB_SCROLL_FWD: return l->scroll_fwd;
    case AU_HIT_BACK: return l->back;
    case AU_HIT_FORWARD: return l->forward;
    case AU_HIT_RELOAD: return l->reload;
    case AU_HIT_HOME: return l->home;
    case AU_HIT_BOOKMARK: return l->bookmark;
    case AU_HIT_THROBBER: return l->throbber;
    case AU_HIT_SPLITTER: return l->splitter;
    case AU_HIT_SECURITY: return l->security;
    default: return (AuIRect){0, 0, 0, 0};
    }
}

/* ---- Layout ---- */

static void set_viewport(AuTab *t)
{
    AuIRect p = t->win->layout.page;
    au_page_set_viewport(t->page, p.x1 - p.x0, p.y1 - p.y0);
}

static void clamp_scroll(AuTab *t)
{
    int mx, my;
    AuBrowserWindow *w = t->win;
    int cw, ch;
    au_page_content_size(t->page, &cw, &ch);
    AuIRect p = w->layout.page;
    mx = cw - (p.x1 - p.x0);
    my = ch - (p.y1 - p.y0);
    if (mx < 0)
        mx = 0;
    if (my < 0)
        my = 0;
    if (t->scroll_x > mx)
        t->scroll_x = mx;
    if (t->scroll_y > my)
        t->scroll_y = my;
    if (t->scroll_x < 0)
        t->scroll_x = 0;
    if (t->scroll_y < 0)
        t->scroll_y = 0;
    au_page_set_scroll(t->page, t->scroll_x, t->scroll_y);
}

/* Recomputes the layout; reports a layout change when anything the shell
 * places natively moved, and repaints everything when full is set. */
static void relayout(AuBrowserWindow *w, bool full)
{
    AuChromeLayout old = w->layout;
    au_chrome_layout(&w->layout, w->width, w->height, w->browser->prefs.tab_position,
                     w->browser->prefs.tab_column_width, w->ntabs, w->tab_scroll);
    w->tab_scroll = w->layout.tab_scroll;
    bool moved = memcmp(&old.page, &w->layout.page, sizeof old.page) ||
                 memcmp(&old.address, &w->layout.address, sizeof old.address) ||
                 memcmp(&old.vscroll, &w->layout.vscroll, sizeof old.vscroll) || old.width != w->width ||
                 old.height != w->height;
    if (moved) {
        AuTab *t = au_bw_active_tab(w);
        if (t) {
            set_viewport(t);
            clamp_scroll(t);
        }
        changed(w, AU_CHANGED_LAYOUT | AU_CHANGED_SCROLL);
        full = true;
    }
    if (full)
        invalidate_all(w);
    else
        invalidate(w, w->layout.tabstrip);
}

static void ensure_tab_visible(AuBrowserWindow *w, int i)
{
    int s = au_chrome_scroll_to_tab(&w->layout, i);
    if (s != w->tab_scroll) {
        w->tab_scroll = s;
        w->layout.tab_scroll = s;
        invalidate(w, w->layout.tabstrip);
    }
}

/* ---- Page callbacks ---- */

static void record_visit(AuBrowser *b, const char *url, const char *title)
{
    if (!b->prefs.history_enabled || !strcmp(url, "about:blank"))
        return;
    au_history_visit(&b->history, url, title, now(b));
}

static void menus_changed_all(AuBrowser *b)
{
    for (int i = 0; i < b->nwindows; i++)
        changed(b->windows[i], AU_CHANGED_MENUS);
}

static void cb_title(void *ctx, AuPage *p, const char *title)
{
    AuTab *t = ctx;
    AuNavEntry *e = au_nav_current(&t->nav);
    if (e)
        set_str(&e->title, title);
    AuBrowser *b = t->win->browser;
    if (b->prefs.history_enabled && *title) {
        au_history_set_title(&b->history, au_page_url(p), title);
        menus_changed_all(b);
    }
    b->session_gen++;
    invalidate_tab(t->win, tab_index(t->win, t));
    if (is_active(t))
        changed(t->win, AU_CHANGED_TITLE | AU_CHANGED_MENUS);
}

static void cb_url(void *ctx, AuPage *p, const char *url)
{
    AuTab *t = ctx;
    AuNavEntry *e = au_nav_current(&t->nav);
    if (e)
        set_str(&e->url, url);
    t->win->browser->session_gen++;
    if (is_active(t) && !t->win->field)
        changed(t->win, AU_CHANGED_FIELD);
    (void)p;
}

static void loading_changed(AuTab *t)
{
    AuBrowserWindow *w = t->win;
    invalidate_tab(w, tab_index(w, t));
    if (is_active(t)) {
        invalidate(w, w->layout.reload);
        invalidate(w, w->layout.throbber);
        invalidate(w, w->layout.status);
        invalidate(w, w->layout.security);
    }
}

static void cb_started(void *ctx, AuPage *p)
{
    AuTab *t = ctx;
    t->loading = true;
    loading_changed(t);
    (void)p;
}

static void cb_progress(void *ctx, AuPage *p)
{
    AuTab *t = ctx;
    if (is_active(t))
        invalidate(t->win, t->win->layout.status);
    (void)p;
}

static void cb_finished(void *ctx, AuPage *p, bool ok)
{
    AuTab *t = ctx;
    t->loading = false;
    loading_changed(t);
    (void)p, (void)ok;
}

static void cb_open(void *ctx, AuPage *p, const char *url, bool new_tab)
{
    AuTab *t = ctx;
    AuBrowserWindow *w = t->win;
    if (new_tab) {
        au_bw_new_tab(w, url, false, tab_index(w, t) + 1);
    } else {
        int i = tab_index(w, t);
        if (i != w->active)
            au_bw_select_tab(w, i);
        au_bw_navigate(w, url);
    }
    (void)p;
}

static void cb_status(void *ctx, AuPage *p, const char *text)
{
    AuTab *t = ctx;
    set_str(&t->status, text);
    if (is_active(t))
        invalidate(t->win, t->win->layout.status);
    (void)p;
}

static void cb_invalidate(void *ctx, AuPage *p, AuIRect r)
{
    AuTab *t = ctx;
    if (!is_active(t))
        return;
    AuIRect pg = t->win->layout.page;
    AuIRect wr = {r.x0 - t->scroll_x + pg.x0, r.y0 - t->scroll_y + pg.y0, r.x1 - t->scroll_x + pg.x0,
                  r.y1 - t->scroll_y + pg.y0};
    invalidate(t->win, au_irect_intersect(wr, pg));
    (void)p;
}

static void cb_content_size(void *ctx, AuPage *p)
{
    AuTab *t = ctx;
    clamp_scroll(t);
    if (is_active(t))
        changed(t->win, AU_CHANGED_SCROLL);
    (void)p;
}

static void rehover(AuBrowserWindow *w);

static void cb_scroll_to(void *ctx, AuPage *p, int x, int y)
{
    AuTab *t = ctx;
    int ox = t->scroll_x, oy = t->scroll_y;
    t->scroll_x = x;
    t->scroll_y = y;
    clamp_scroll(t);
    if (is_active(t) && (t->scroll_x != ox || t->scroll_y != oy)) {
        invalidate(t->win, t->win->layout.page);
        changed(t->win, AU_CHANGED_SCROLL);
        rehover(t->win);
    }
    (void)p;
}

static uint32_t env_time(void *ctx)
{
    return time_ms(ctx);
}

static bool env_read(void *ctx, const char *url, char **data, size_t *len)
{
    AuBrowser *b = ctx;
    return b->host.read_file_url && b->host.read_file_url(b->host.ctx, url, data, len);
}

static const AuPageFetcher *env_fetcher(void *ctx, const char *scheme)
{
    AuBrowser *b = ctx;
    for (int i = 0; i < b->nfetchers; i++)
        if (!strcmp(b->fetchers[i].scheme, scheme))
            return &b->fetchers[i].fetcher;
    return NULL;
}

void au_browser_set_fetcher(AuBrowser *b, const char *scheme, const AuPageFetcher *f)
{
    for (int i = 0; i < b->nfetchers; i++) {
        if (!strcmp(b->fetchers[i].scheme, scheme)) {
            if (f) {
                b->fetchers[i].fetcher = *f;
            } else {
                b->fetchers[i] = b->fetchers[b->nfetchers - 1];
                b->nfetchers--;
            }
            return;
        }
    }
    if (!f || b->nfetchers == AU_MAX_FETCHERS || strlen(scheme) >= sizeof b->fetchers[0].scheme)
        return;
    snprintf(b->fetchers[b->nfetchers].scheme, sizeof b->fetchers[0].scheme, "%s", scheme);
    b->fetchers[b->nfetchers].fetcher = *f;
    b->nfetchers++;
}

/* ---- Browser ---- */

AuBrowser *au_browser_new(AuFontSet *fonts, const AuBrowserHost *host)
{
    AuBrowser *b = au_calloc(sizeof *b);
    au_prefs_init(&b->prefs);
    au_history_init(&b->history, AU_HISTORY_DEFAULT_MAX);
    au_bookmarks_init(&b->bookmarks);
    au_downloads_init(&b->downloads, au_sim_source());
    b->fonts = fonts;
    b->host = *host;
    b->next_tab_id = 1;
    return b;
}

void au_browser_free(AuBrowser *b)
{
    while (b->nwindows)
        au_browser_window_free(b->windows[b->nwindows - 1]);
    au_free(b->windows);
    au_prefs_free(&b->prefs);
    au_history_free(&b->history);
    au_bookmarks_free(&b->bookmarks);
    au_downloads_free(&b->downloads);
    au_free(b);
}

void au_browser_prefs_changed(AuBrowser *b)
{
    b->prefs_gen++;
    for (int i = 0; i < b->nwindows; i++) {
        AuBrowserWindow *w = b->windows[i];
        relayout(w, true);
        /* Restyle pages for changed font settings. */
        for (int j = 0; j < w->ntabs; j++)
            au_page_prefs_changed(w->tabs[j]->page);
        AuTab *t = au_bw_active_tab(w);
        if (t) {
            set_viewport(t);
            clamp_scroll(t);
        }
        changed(w, AU_CHANGED_LAYOUT | AU_CHANGED_SCROLL | AU_CHANGED_FIELD);
    }
}

void au_browser_bookmarks_changed(AuBrowser *b)
{
    for (int i = 0; i < b->nwindows; i++) {
        invalidate(b->windows[i], b->windows[i]->layout.bookmark);
        changed(b->windows[i], AU_CHANGED_MENUS);
    }
}

void au_browser_history_changed(AuBrowser *b)
{
    menus_changed_all(b);
}

bool au_browser_busy(const AuBrowser *b)
{
    for (int i = 0; i < b->nwindows; i++)
        for (int j = 0; j < b->windows[i]->ntabs; j++)
            if (au_page_busy(b->windows[i]->tabs[j]->page))
                return true;
    return false;
}

AuBrowserWindow *au_browser_window_new(AuBrowser *b, int width, int height)
{
    AuBrowserWindow *w = au_calloc(sizeof *w);
    w->browser = b;
    w->width = width;
    w->height = height;
    w->active = -1;
    w->drag_tab = -1;
    w->hover.part = w->pressed.part = AU_HIT_NONE;
    w->active_window = true;
    b->windows = au_realloc(b->windows, (size_t)(b->nwindows + 1) * sizeof *b->windows);
    b->windows[b->nwindows++] = w;
    au_chrome_layout(&w->layout, width, height, b->prefs.tab_position, b->prefs.tab_column_width, 0, 0);
    b->session_gen++;
    return w;
}

static void free_tab(AuTab *t)
{
    au_page_free(t->page);
    au_nav_free(&t->nav);
    au_free(t->status);
    au_free(t);
}

void au_browser_window_free(AuBrowserWindow *w)
{
    AuBrowser *b = w->browser;
    for (int i = 0; i < w->ntabs; i++)
        free_tab(w->tabs[i]);
    au_free(w->tabs);
    au_free(w->field);
    for (int i = 0; i < b->nwindows; i++) {
        if (b->windows[i] == w) {
            memmove(&b->windows[i], &b->windows[i + 1], (size_t)(b->nwindows - i - 1) * sizeof *b->windows);
            b->nwindows--;
            break;
        }
    }
    b->session_gen++;
    au_free(w);
}

/* ---- Tabs ---- */

AuTab *au_bw_active_tab(AuBrowserWindow *w)
{
    return w->active >= 0 && w->active < w->ntabs ? w->tabs[w->active] : NULL;
}

static AuTab *make_tab(AuBrowserWindow *w, int index)
{
    AuBrowser *b = w->browser;
    AuTab *t = au_calloc(sizeof *t);
    t->id = b->next_tab_id++;
    t->win = w;
    t->status = au_strndup("", 0);
    au_nav_init(&t->nav);
    AuPageEnv env = {b->fonts, &b->prefs, b, env_time, env_read, env_fetcher};
    AuPageCallbacks cb = {t,           cb_title, cb_url,    cb_started,    cb_progress,     cb_finished,
                          cb_open,     cb_status, cb_invalidate, cb_content_size, cb_scroll_to};
    t->page = au_page_new(&env, &cb);
    if (index < 0 || index > w->ntabs)
        index = w->ntabs;
    if (w->ntabs == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 8;
        w->tabs = au_realloc(w->tabs, (size_t)w->cap * sizeof *w->tabs);
    }
    memmove(&w->tabs[index + 1], &w->tabs[index], (size_t)(w->ntabs - index) * sizeof *w->tabs);
    w->tabs[index] = t;
    w->ntabs++;
    if (w->active >= index)
        w->active++;
    set_viewport(t);
    b->session_gen++;
    return t;
}

/* Loads url into tab t as a new back/forward entry. */
static void load_new(AuTab *t, const char *url)
{
    AuNavEntry *cur = au_nav_current(&t->nav);
    if (cur)
        cur->scroll_y = t->scroll_y;
    au_nav_push(&t->nav, url, "");
    t->scroll_x = t->scroll_y = 0;
    au_page_set_scroll(t->page, 0, 0);
    record_visit(t->win->browser, url, NULL);
    t->win->browser->session_gen++;
    au_page_navigate(t->page, url);
}

AuTab *au_bw_new_tab(AuBrowserWindow *w, const char *url, bool activate, int index)
{
    if (index < 0)
        index = w->active >= 0 ? w->active + 1 : w->ntabs;
    AuTab *t = make_tab(w, index);
    relayout(w, false);
    if (activate || w->active < 0)
        au_bw_select_tab(w, tab_index(w, t));
    load_new(t, url && *url ? url : "about:blank");
    changed(w, AU_CHANGED_MENUS);
    return t;
}

void au_bw_select_tab(AuBrowserWindow *w, int i)
{
    if (i < 0 || i >= w->ntabs)
        return;
    int old = w->active;
    w->active = i;
    au_free(w->field);
    w->field = NULL;
    AuTab *t = w->tabs[i];
    set_viewport(t);
    clamp_scroll(t);
    ensure_tab_visible(w, i);
    if (old != i) {
        invalidate_tab(w, old);
        invalidate_tab(w, i);
        invalidate(w, w->layout.toolbar);
        invalidate(w, w->layout.page);
        invalidate(w, w->layout.status);
        w->browser->session_gen++;
    }
    changed(w, AU_CHANGED_FIELD | AU_CHANGED_TITLE | AU_CHANGED_SCROLL | AU_CHANGED_MENUS);
}

void au_bw_close_tab(AuBrowserWindow *w, int i)
{
    if (i < 0 || i >= w->ntabs)
        return;
    if (w->ntabs == 1) {
        AuBrowserHost *h = &w->browser->host;
        if (h->close_window)
            h->close_window(h->ctx, w);
        return;
    }
    AuTab *t = w->tabs[i];
    memmove(&w->tabs[i], &w->tabs[i + 1], (size_t)(w->ntabs - i - 1) * sizeof *w->tabs);
    w->ntabs--;
    free_tab(t);
    int next = w->active;
    if (i < w->active)
        next = w->active - 1;
    else if (i == w->active)
        next = i < w->ntabs ? i : w->ntabs - 1; /* the tab to the right, else left */
    w->active = -1;
    w->hover.part = w->pressed.part = AU_HIT_NONE;
    relayout(w, true);
    au_bw_select_tab(w, next);
    w->browser->session_gen++;
}

void au_bw_move_tab(AuBrowserWindow *w, int from, int to)
{
    if (from < 0 || from >= w->ntabs || to < 0 || to >= w->ntabs || from == to)
        return;
    AuTab *t = w->tabs[from];
    AuTab *act = au_bw_active_tab(w);
    if (from < to)
        memmove(&w->tabs[from], &w->tabs[from + 1], (size_t)(to - from) * sizeof *w->tabs);
    else
        memmove(&w->tabs[to + 1], &w->tabs[to], (size_t)(from - to) * sizeof *w->tabs);
    w->tabs[to] = t;
    w->active = act ? tab_index(w, act) : -1;
    invalidate(w, w->layout.tabstrip);
    w->browser->session_gen++;
    changed(w, AU_CHANGED_MENUS);
}

/* ---- Navigation ---- */

void au_bw_navigate(AuBrowserWindow *w, const char *url)
{
    AuTab *t = au_bw_active_tab(w);
    if (!t)
        return;
    au_free(w->field);
    w->field = NULL;
    load_new(t, url);
    invalidate(w, w->layout.toolbar);
    changed(w, AU_CHANGED_FIELD | AU_CHANGED_SCROLL | AU_CHANGED_MENUS);
}

void au_bw_navigate_input(AuBrowserWindow *w, const char *text)
{
    char *url = au_url_from_input(text, w->browser->prefs.search_template, NULL);
    if (url) {
        au_bw_navigate(w, url);
        au_free(url);
    }
}

void au_bw_go(AuBrowserWindow *w, int delta)
{
    AuTab *t = au_bw_active_tab(w);
    if (!t || !au_nav_can_go(&t->nav, delta))
        return;
    AuNavEntry *cur = au_nav_current(&t->nav);
    if (cur)
        cur->scroll_y = t->scroll_y;
    AuNavEntry *e = au_nav_go(&t->nav, delta);
    char *url = au_strndup(e->url, strlen(e->url));
    int sy = e->scroll_y;
    au_free(w->field);
    w->field = NULL;
    record_visit(w->browser, url, e->title);
    /* The page scrolls back to sy once it is loaded far enough. */
    t->scroll_x = t->scroll_y = 0;
    au_page_set_scroll(t->page, 0, 0);
    au_page_navigate_entry(t->page, url, 0, sy);
    au_free(url);
    clamp_scroll(t);
    w->browser->session_gen++;
    invalidate(w, w->layout.toolbar);
    invalidate(w, w->layout.page);
    changed(w, AU_CHANGED_FIELD | AU_CHANGED_SCROLL | AU_CHANGED_MENUS);
}

void au_bw_back(AuBrowserWindow *w) { au_bw_go(w, -1); }
void au_bw_forward(AuBrowserWindow *w) { au_bw_go(w, 1); }

void au_bw_reload(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    if (t)
        au_page_reload(t->page);
}

void au_bw_stop(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    if (t)
        au_page_stop(t->page);
}

void au_bw_home(AuBrowserWindow *w)
{
    const char *home = w->browser->prefs.home_page;
    au_bw_navigate_input(w, home && *home ? home : "about:home");
}

bool au_bw_is_bookmarked(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    return t && au_bookmark_find_url(&w->browser->bookmarks, au_page_url(t->page)) != NULL;
}

AuBookmark *au_bw_bookmark_page(AuBrowserWindow *w, bool toggle)
{
    AuTab *t = au_bw_active_tab(w);
    if (!t)
        return NULL;
    AuBrowser *b = w->browser;
    const char *url = au_page_url(t->page);
    AuBookmark *have = au_bookmark_find_url(&b->bookmarks, url);
    AuBookmark *made = NULL;
    if (have) {
        if (!toggle)
            return have;
        au_bookmark_delete(&b->bookmarks, have);
    } else {
        const char *title = au_page_title(t->page);
        made = au_bookmark_add(&b->bookmarks, NULL, NULL, *title ? title : url, url, now(b));
    }
    au_browser_bookmarks_changed(b);
    return made;
}

/* ---- Address field ---- */

const char *au_bw_field_text(AuBrowserWindow *w)
{
    if (w->field)
        return w->field;
    AuTab *t = au_bw_active_tab(w);
    if (!t)
        return "";
    const char *u = au_page_url(t->page);
    return strcmp(u, "about:blank") ? u : "";
}

void au_bw_set_field_text(AuBrowserWindow *w, const char *text)
{
    au_free(w->field);
    w->field = text ? au_strndup(text, strlen(text)) : NULL;
    if (!w->native_field)
        invalidate(w, w->layout.address);
    if (!text)
        changed(w, AU_CHANGED_FIELD);
}

void au_bw_set_field_focus(AuBrowserWindow *w, bool focused)
{
    if (w->field_focused == focused)
        return;
    w->field_focused = focused;
    AuIRect r = w->layout.address;
    r.x0 -= 3;
    r.y0 -= 3;
    r.x1 += 3;
    r.y1 += 3;
    invalidate(w, r);
}

const char *au_bw_complete(AuBrowserWindow *w, const char *typed)
{
    const AuHistoryEntry *h = au_history_complete(&w->browser->history, typed);
    if (h)
        return h->url;
    const AuBookmark *b = au_bookmarks_complete(&w->browser->bookmarks, typed);
    return b ? b->url : NULL;
}

/* ---- Geometry and painting ---- */

void au_bw_resize(AuBrowserWindow *w, int width, int height)
{
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;
    w->width = width;
    w->height = height;
    relayout(w, true);
}

void au_bw_set_active(AuBrowserWindow *w, bool active)
{
    if (w->active_window == active)
        return;
    w->active_window = active;
    w->hover.part = AU_HIT_NONE;
    invalidate(w, w->layout.tabstrip);
}

void au_bw_paint(AuBrowserWindow *w, AuCanvas *c, AuIRect clip)
{
    clip = au_irect_intersect(clip, (AuIRect){0, 0, w->width, w->height});
    if (au_irect_empty(clip))
        return;
    AuTab *t = au_bw_active_tab(w);
    AuChromeTab stack_tabs[16];
    AuChromeTab *tabs = w->ntabs <= 16 ? stack_tabs : au_alloc((size_t)w->ntabs * sizeof *tabs);
    for (int i = 0; i < w->ntabs; i++) {
        AuTab *x = w->tabs[i];
        const char *title = au_page_title(x->page);
        if (!*title) {
            const char *u = au_page_url(x->page);
            title = strcmp(u, "about:blank") ? u : "New Tab";
        }
        tabs[i].title = title;
        tabs[i].loading = x->loading;
    }
    AuChromeState s;
    memset(&s, 0, sizeof s);
    s.tabs = tabs;
    s.active = w->active;
    s.hover = w->hover;
    s.pressed = w->pressed;
    s.dragging = w->drag_tab;
    s.can_back = t && au_nav_can_go(&t->nav, -1);
    s.can_forward = t && au_nav_can_go(&t->nav, 1);
    s.loading = t && t->loading;
    s.bookmarked = au_bw_is_bookmarked(w);
    s.security = t ? au_page_security(t->page) : AU_SECURITY_NONE;
    s.field_text = au_bw_field_text(w);
    s.field_focused = w->field_focused;
    s.draw_field_text = !w->native_field;
    s.status_text = t ? t->status : "";
    char status[300];
    if (t && t->loading && !*t->status) {
        snprintf(status, sizeof status, "Loading %s", au_page_url(t->page));
        s.status_text = status;
    }
    s.progress = t ? au_page_progress(t->page) : 0;
    s.throbber_frame = w->throbber_frame;
    s.draw_native_standins = w->native_standins;
    s.active_window = w->active_window;
    au_chrome_paint(c, w->browser->fonts, &w->layout, &s, clip);
    AuIRect pg = au_irect_intersect(w->layout.page, clip);
    if (t && !au_irect_empty(pg)) {
        AuIRect p = w->layout.page;
        au_canvas_clip_push(c, (AuRect){(float)pg.x0, (float)pg.y0, (float)(pg.x1 - pg.x0), (float)(pg.y1 - pg.y0)});
        au_page_paint(t->page, c, (AuRect){(float)p.x0, (float)p.y0, (float)(p.x1 - p.x0), (float)(p.y1 - p.y0)},
                      t->scroll_x, t->scroll_y);
        au_canvas_clip_pop(c);
    } else if (!au_irect_empty(pg)) {
        au_fill_rect(c, (AuRect){(float)pg.x0, (float)pg.y0, (float)(pg.x1 - pg.x0), (float)(pg.y1 - pg.y0)},
                     AU_RGB(255, 255, 255));
    }
    if (tabs != stack_tabs)
        au_free(tabs);
}

void au_bw_window_title(AuBrowserWindow *w, char *out, size_t cap)
{
    AuTab *t = au_bw_active_tab(w);
    const char *title = t ? au_page_title(t->page) : "";
    if (!*title && t) {
        const char *u = au_page_url(t->page);
        title = strcmp(u, "about:blank") ? u : "";
    }
    if (*title)
        snprintf(out, cap, "%s", title);
    else
        snprintf(out, cap, "Aura");
}

/* ---- Scrolling ---- */

void au_bw_scroll_max(AuBrowserWindow *w, int *max_x, int *max_y)
{
    *max_x = *max_y = 0;
    AuTab *t = au_bw_active_tab(w);
    if (!t)
        return;
    int cw, ch;
    au_page_content_size(t->page, &cw, &ch);
    AuIRect p = w->layout.page;
    *max_x = cw - (p.x1 - p.x0);
    *max_y = ch - (p.y1 - p.y0);
    if (*max_x < 0)
        *max_x = 0;
    if (*max_y < 0)
        *max_y = 0;
}

static void page_event(AuBrowserWindow *w, const AuEvent *ev);

/* The document moved under a resting pointer: hover and the status text
 * follow what is now under it. */
static void rehover(AuBrowserWindow *w)
{
    if (!w->mouse_in_page || w->pressed.part != AU_HIT_NONE)
        return;
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_MOUSE_MOVE;
    e.x = w->mouse_x;
    e.y = w->mouse_y;
    page_event(w, &e);
}

void au_bw_scroll_to(AuBrowserWindow *w, int x, int y)
{
    AuTab *t = au_bw_active_tab(w);
    if (!t)
        return;
    int ox = t->scroll_x, oy = t->scroll_y;
    t->scroll_x = x;
    t->scroll_y = y;
    clamp_scroll(t);
    if (t->scroll_x != ox || t->scroll_y != oy) {
        invalidate(w, w->layout.page);
        changed(w, AU_CHANGED_SCROLL);
        rehover(w);
    }
}

void au_bw_scroll_by(AuBrowserWindow *w, int dx, int dy)
{
    AuTab *t = au_bw_active_tab(w);
    if (t)
        au_bw_scroll_to(w, t->scroll_x + dx, t->scroll_y + dy);
}

/* ---- Input ---- */

static bool same_hit(AuHit a, AuHit b)
{
    return a.part == b.part && a.index == b.index;
}

static void set_hover(AuBrowserWindow *w, AuHit h)
{
    if (same_hit(h, w->hover))
        return;
    invalidate(w, au_bw_hit_rect(w, w->hover));
    w->hover = h;
    invalidate(w, au_bw_hit_rect(w, h));
}

static void page_event(AuBrowserWindow *w, const AuEvent *ev)
{
    AuTab *t = au_bw_active_tab(w);
    if (!t)
        return;
    AuEvent e = *ev;
    e.x = ev->x - w->layout.page.x0 + t->scroll_x;
    e.y = ev->y - w->layout.page.y0 + t->scroll_y;
    au_page_mouse(t->page, &e);
}

static void click(AuBrowserWindow *w, AuHit h, const AuEvent *ev)
{
    AuTab *t = au_bw_active_tab(w);
    switch (h.part) {
    case AU_HIT_BACK: au_bw_back(w); break;
    case AU_HIT_FORWARD: au_bw_forward(w); break;
    case AU_HIT_RELOAD:
        if (t && t->loading)
            au_bw_stop(w);
        else
            au_bw_reload(w);
        break;
    case AU_HIT_HOME: au_bw_home(w); break;
    case AU_HIT_BOOKMARK: au_bw_bookmark_page(w, true); break;
    case AU_HIT_NEW_TAB:
        au_bw_new_tab(w, NULL, true, w->ntabs);
        changed(w, AU_CHANGED_FOCUS_FIELD);
        break;
    case AU_HIT_TAB_CLOSE: au_bw_close_tab(w, h.index); break;
    default: break;
    }
    (void)ev;
}

AuHit au_bw_mouse(AuBrowserWindow *w, const AuEvent *ev)
{
    AuHit h = au_chrome_hit(&w->layout, ev->x, ev->y);
    if (ev->type == AU_EVENT_MOUSE_MOVE) {
        w->mouse_x = ev->x;
        w->mouse_y = ev->y;
        w->mouse_in_page = h.part == AU_HIT_PAGE;
    }
    switch (ev->type) {
    case AU_EVENT_MOUSE_MOVE:
        if (w->pressed.part == AU_HIT_SPLITTER) {
            int nw = w->split_w + ev->x - w->split_x;
            AuPrefs *p = &w->browser->prefs;
            if (nw < AU_TAB_COLUMN_MIN)
                nw = AU_TAB_COLUMN_MIN;
            if (nw > AU_TAB_COLUMN_MAX)
                nw = AU_TAB_COLUMN_MAX;
            if (nw != p->tab_column_width) {
                p->tab_column_width = nw;
                relayout(w, false);
            }
            return w->pressed;
        }
        if (w->drag_tab >= 0) {
            int dx = ev->x - w->drag_x, dy = ev->y - w->drag_y;
            if (!w->drag_moved && dx * dx + dy * dy >= 16)
                w->drag_moved = true;
            if (w->drag_moved) {
                int to = au_chrome_drop_index(&w->layout, ev->x, ev->y);
                if (to != w->drag_tab) {
                    au_bw_move_tab(w, w->drag_tab, to);
                    w->drag_tab = to;
                    w->pressed.index = to;
                }
            }
            return w->pressed;
        }
        if (w->pressed.part != AU_HIT_NONE && w->pressed.part != AU_HIT_PAGE) {
            /* Tracking a button: it shows pressed only while hovered. */
            set_hover(w, h);
            return h;
        }
        set_hover(w, h.part == AU_HIT_PAGE ? (AuHit){AU_HIT_NONE, -1} : h);
        if (h.part == AU_HIT_PAGE || w->pressed.part == AU_HIT_PAGE)
            page_event(w, ev);
        else {
            AuTab *t = au_bw_active_tab(w);
            if (t && *t->status) {
                /* Left the page: clear a link's status text. */
                AuEvent out = *ev;
                out.x = out.y = -100000;
                page_event(w, &out);
            }
        }
        return h;
    case AU_EVENT_MOUSE_DOWN:
        w->pressed = h;
        w->hover = h;
        switch (h.part) {
        case AU_HIT_TAB:
            au_bw_select_tab(w, h.index);
            w->drag_tab = h.index;
            w->drag_x = ev->x;
            w->drag_y = ev->y;
            w->drag_moved = false;
            break;
        case AU_HIT_TAB_SCROLL_BACK:
        case AU_HIT_TAB_SCROLL_FWD: {
            int d = h.part == AU_HIT_TAB_SCROLL_BACK ? -w->layout.tab_size : w->layout.tab_size;
            w->tab_scroll += d;
            relayout(w, false);
            break;
        }
        case AU_HIT_SPLITTER:
            w->split_x = ev->x;
            w->split_w = w->browser->prefs.tab_column_width;
            break;
        case AU_HIT_PAGE: page_event(w, ev); break;
        default: break;
        }
        invalidate(w, au_bw_hit_rect(w, h));
        return h;
    case AU_EVENT_MOUSE_UP: {
        AuHit p = w->pressed;
        w->pressed.part = AU_HIT_NONE;
        w->pressed.index = -1;
        if (w->drag_tab >= 0) {
            w->drag_tab = -1;
            invalidate(w, w->layout.tabstrip);
        }
        if (p.part == AU_HIT_SPLITTER) {
            /* The width is a preference: other windows follow. */
            au_browser_prefs_changed(w->browser);
            return p;
        }
        if (p.part == AU_HIT_PAGE) {
            page_event(w, ev);
            return p;
        }
        invalidate(w, au_bw_hit_rect(w, p));
        if (same_hit(p, h))
            click(w, h, ev);
        return h;
    }
    case AU_EVENT_SCROLL: {
        /* An inner scrolling box under the pointer takes the wheel first. */
        AuTab *t = au_bw_active_tab(w);
        if (t && h.part == AU_HIT_PAGE &&
            au_page_scroll(t->page, ev->x - w->layout.page.x0 + t->scroll_x, ev->y - w->layout.page.y0 + t->scroll_y,
                           ev->dx, ev->dy))
            return h;
        au_bw_scroll_by(w, ev->dx, ev->dy);
        return h;
    }
    default:
        return h;
    }
}

bool au_bw_key(AuBrowserWindow *w, const AuEvent *ev)
{
    if (ev->type != AU_EVENT_KEY_DOWN)
        return false;
    AuTab *t = au_bw_active_tab(w);
    if (!t)
        return false;
    if (au_page_key(t->page, ev))
        return true;
    AuIRect p = w->layout.page;
    int page = (p.y1 - p.y0) - 40;
    if (page < 20)
        page = 20;
    switch (ev->key) {
    case AU_KEY_UP: au_bw_scroll_by(w, 0, -16); return true;
    case AU_KEY_DOWN: au_bw_scroll_by(w, 0, 16); return true;
    case AU_KEY_LEFT: au_bw_scroll_by(w, -16, 0); return true;
    case AU_KEY_RIGHT: au_bw_scroll_by(w, 16, 0); return true;
    case AU_KEY_PAGE_UP: au_bw_scroll_by(w, 0, -page); return true;
    case AU_KEY_PAGE_DOWN: au_bw_scroll_by(w, 0, page); return true;
    case AU_KEY_HOME: au_bw_scroll_to(w, 0, 0); return true;
    case AU_KEY_END: au_bw_scroll_to(w, 0, 1 << 30); return true;
    default: break;
    }
    if (ev->ch == ' ') {
        au_bw_scroll_by(w, 0, (ev->mods & AU_MOD_SHIFT) ? -page : page);
        return true;
    }
    return false;
}

void au_bw_idle(AuBrowserWindow *w)
{
    bool any = false;
    for (int i = 0; i < w->ntabs; i++) {
        au_page_idle(w->tabs[i]->page);
        if (i < w->ntabs && w->tabs[i]->loading)
            any = true;
    }
    if (!any)
        return;
    uint32_t t = time_ms(w->browser);
    if (t - w->throbber_ms < 110)
        return;
    w->throbber_ms = t;
    w->throbber_frame = (w->throbber_frame + 1) & 7;
    AuTab *act = au_bw_active_tab(w);
    if (act && act->loading)
        invalidate(w, w->layout.throbber);
    for (int i = 0; i < w->ntabs; i++) {
        if (!w->tabs[i]->loading)
            continue;
        AuIRect r = au_chrome_tab_rect(&w->layout, i);
        /* Only the spinner at the tab's left. */
        if (w->layout.position == AU_TABS_TOP)
            r = (AuIRect){r.x0 + 2, r.y0 + 2, r.x0 + 22, r.y1 - 1};
        else
            r = (AuIRect){r.x0 + 4, r.y0, r.x0 + 24, r.y1};
        invalidate(w, au_irect_intersect(r, w->layout.tabs_area));
    }
}

/* ---- Session ---- */

static void put_clean(AuBuf *out, const char *s)
{
    for (; *s; s++)
        au_buf_putc(out, (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s);
}

void au_browser_session_serialize(const AuBrowser *b, AuBuf *out)
{
    au_buf_puts(out, "# Aura session 1\n");
    for (int i = 0; i < b->nwindows; i++) {
        const AuBrowserWindow *w = b->windows[i];
        au_buf_printf(out, "window %d %d %d\n", w->width, w->height, w->active);
        for (int j = 0; j < w->ntabs; j++) {
            const AuNavList *nav = &w->tabs[j]->nav;
            au_buf_printf(out, "tab %d\n", nav->index);
            for (int k = 0; k < nav->n; k++) {
                au_buf_puts(out, "entry\t");
                put_clean(out, nav->e[k].url);
                au_buf_putc(out, '\t');
                put_clean(out, nav->e[k].title);
                au_buf_putc(out, '\n');
            }
        }
    }
}

void au_session_free(AuSession *s)
{
    for (int i = 0; i < s->nwindows; i++) {
        for (int j = 0; j < s->windows[i].ntabs; j++)
            au_nav_free(&s->windows[i].tabs[j].nav);
        au_free(s->windows[i].tabs);
    }
    au_free(s->windows);
    memset(s, 0, sizeof *s);
}

bool au_session_parse(AuSession *s, const char *data, size_t len)
{
    memset(s, 0, sizeof *s);
    bool ok = true;
    AuBuf line;
    au_buf_init(&line);
    size_t i = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && data[e] != '\n' && data[e] != '\r')
            e++;
        au_buf_clear(&line);
        au_buf_append(&line, data + i, e - i);
        char *l = au_buf_cstr(&line);
        i = e;
        while (i < len && (data[i] == '\n' || data[i] == '\r'))
            i++;
        if (!*l || *l == '#')
            continue;
        if (memchr(l, 0, line.len)) {
            ok = false;
            continue;
        }
        if (!strncmp(l, "window ", 7)) {
            int w = 0, h = 0, a = 0;
            if (sscanf(l + 7, "%d %d %d", &w, &h, &a) != 3) {
                ok = false;
                continue;
            }
            s->windows = au_realloc(s->windows, (size_t)(s->nwindows + 1) * sizeof *s->windows);
            AuSessionWindow *sw = &s->windows[s->nwindows++];
            memset(sw, 0, sizeof *sw);
            sw->width = w;
            sw->height = h;
            sw->active = a;
        } else if (!strncmp(l, "tab ", 4) && s->nwindows) {
            AuSessionWindow *sw = &s->windows[s->nwindows - 1];
            int idx = 0;
            if (sscanf(l + 4, "%d", &idx) != 1) {
                ok = false;
                continue;
            }
            sw->tabs = au_realloc(sw->tabs, (size_t)(sw->ntabs + 1) * sizeof *sw->tabs);
            AuSessionTab *st = &sw->tabs[sw->ntabs++];
            au_nav_init(&st->nav);
            st->nav.index = idx; /* checked after its entries are read */
        } else if (!strncmp(l, "entry\t", 6) && s->nwindows && s->windows[s->nwindows - 1].ntabs) {
            AuSessionWindow *sw = &s->windows[s->nwindows - 1];
            AuNavList *nav = &sw->tabs[sw->ntabs - 1].nav;
            char *url = l + 6;
            char *tab = strchr(url, '\t');
            if (tab)
                *tab = 0;
            if (!*url) {
                ok = false;
                continue;
            }
            int want = nav->index;
            nav->index = nav->n - 1;
            au_nav_push(nav, url, tab ? tab + 1 : "");
            nav->index = want;
        } else {
            ok = false;
        }
    }
    au_buf_free(&line);
    /* Drop empty tabs and fix indices. */
    for (int wi = 0; wi < s->nwindows; wi++) {
        AuSessionWindow *sw = &s->windows[wi];
        int k = 0;
        for (int j = 0; j < sw->ntabs; j++) {
            AuNavList *nav = &sw->tabs[j].nav;
            if (nav->n == 0) {
                au_nav_free(nav);
                ok = false;
                continue;
            }
            if (nav->index < 0 || nav->index >= nav->n)
                nav->index = nav->n - 1;
            sw->tabs[k++] = sw->tabs[j];
        }
        sw->ntabs = k;
        if (sw->active < 0 || sw->active >= sw->ntabs)
            sw->active = 0;
    }
    return ok;
}

void au_bw_restore(AuBrowserWindow *w, const AuSessionWindow *sw)
{
    int first = w->ntabs;
    for (int j = 0; j < sw->ntabs; j++) {
        const AuNavList *src = &sw->tabs[j].nav;
        AuTab *t = make_tab(w, w->ntabs);
        for (int k = 0; k < src->n; k++)
            au_nav_push(&t->nav, src->e[k].url, src->e[k].title);
        t->nav.index = src->index;
        AuNavEntry *e = au_nav_current(&t->nav);
        if (e) {
            char *url = au_strndup(e->url, strlen(e->url));
            au_page_navigate(t->page, url);
            au_free(url);
        }
    }
    relayout(w, true);
    if (sw->ntabs)
        au_bw_select_tab(w, first + sw->active);
}
