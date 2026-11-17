/* The browser model and controller. See browser.h. */
#include "browser.h"
#include "bench.h"
#include "favicon.h"
#include "scriptmem.h"
#include "url.h"
#include "../net/web_storage.h"

#include <stdio.h>
#include <stdlib.h>
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
    e->id = ++l->next_id;
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

/* The active tab t scrolled from (ox, oy) to its current position. When
 * may_shift is set and the shell can move pixels (scroll_area), the page
 * says which areas besides the exposed strips are then wrong (fixed and
 * sticky boxes; au_page_scroll_damage): the shell moves the pixels and
 * only those areas and the strips are painted. Otherwise the whole page
 * area is. */
static void page_scrolled(AuBrowserWindow *w, AuTab *t, int ox, int oy, bool may_shift)
{
    AuBrowserHost *h = &w->browser->host;
    AuIRect pg = w->layout.page;
    int nx = t->scroll_x, ny = t->scroll_y;
    int dx = ox - nx, dy = oy - ny; /* how the pixels move */
    int pw = pg.x1 - pg.x0, ph = pg.y1 - pg.y0;
    if (!may_shift || !h->scroll_area || w->painting || au_irect_empty(pg) || abs(dx) >= pw || abs(dy) >= ph) {
        invalidate(w, pg);
        return;
    }
    AuIRect boxes[AU_PAGE_SCROLL_DAMAGE];
    int n = au_page_scroll_damage(t->page, ox, oy, nx, ny, boxes, AU_PAGE_SCROLL_DAMAGE);
    if (n < 0 || t->scroll_x != nx || t->scroll_y != ny) {
        invalidate(w, pg);
        return;
    }
    h->scroll_area(h->ctx, w, pg, dx, dy);
    for (int i = 0; i < n; i++) {
        AuIRect r = {boxes[i].x0 + pg.x0, boxes[i].y0 + pg.y0, boxes[i].x1 + pg.x0, boxes[i].y1 + pg.y0};
        invalidate(w, au_irect_intersect(r, pg));
    }
    if (dy > 0)
        invalidate(w, (AuIRect){pg.x0, pg.y0, pg.x1, pg.y0 + dy});
    else if (dy < 0)
        invalidate(w, (AuIRect){pg.x0, pg.y1 + dy, pg.x1, pg.y1});
    if (dx > 0)
        invalidate(w, (AuIRect){pg.x0, pg.y0, pg.x0 + dx, pg.y1});
    else if (dx < 0)
        invalidate(w, (AuIRect){pg.x1 + dx, pg.y0, pg.x1, pg.y1});
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
    if (au_page_discarded(t->page))
        return; /* an unloaded tab keeps the position its document had */
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
    if (w->native_chrome)
        au_chrome_layout_page_only(&w->layout, w->width, w->height, w->ntabs);
    else
        au_chrome_layout(&w->layout, w->width, w->height, w->browser->prefs.tab_position,
                         w->browser->prefs.tab_column_width, w->ntabs, w->tab_scroll);
    w->tab_scroll = w->layout.tab_scroll;
    /* A horizontal scroll bar while the active page is wider than the
     * page area (its width does not change with the bar). In native
     * chrome the port's scroll bars lie outside the page's surface. */
    AuTab *at = au_bw_active_tab(w);
    if (at && !w->native_chrome) {
        int cw, ch;
        au_page_content_size(at->page, &cw, &ch);
        int pw = w->layout.page.x1 - w->layout.page.x0;
        bool want = cw > pw;
        /* When the page area's width changes, the content width known here
         * is the old one: a page that fitted will usually fit again, so
         * the bar stays only where it was shown. A wrong guess costs a
         * second layout (check_hscroll); guessing from the old width was
         * wrong on every resize of a page that follows the window. */
        if (pw != old.page.x1 - old.page.x0 && au_irect_empty(old.hscroll))
            want = false;
        if (want)
            au_chrome_layout_hscroll(&w->layout);
    }
    bool moved = memcmp(&old.page, &w->layout.page, sizeof old.page) ||
                 memcmp(&old.address, &w->layout.address, sizeof old.address) ||
                 memcmp(&old.vscroll, &w->layout.vscroll, sizeof old.vscroll) ||
                 memcmp(&old.hscroll, &w->layout.hscroll, sizeof old.hscroll) || old.width != w->width ||
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

/* Lays out again when the active page's width needs the horizontal
 * scroll bar shown or hidden. */
static void check_hscroll(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    if (!t || w->native_chrome)
        return;
    int cw, ch;
    au_page_content_size(t->page, &cw, &ch);
    bool need = cw > w->layout.page.x1 - w->layout.page.x0;
    if (need != !au_irect_empty(w->layout.hscroll))
        relayout(w, false);
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
    changed(t->win, AU_CHANGED_TAB_INFO | (is_active(t) ? AU_CHANGED_TITLE | AU_CHANGED_MENUS : 0));
}

/* A redirect replaced the address of the load in progress: the history
 * keeps the final address only (the visit load_new recorded for the
 * first address goes, unless it was visited before). */
static void forget_visit(AuBrowser *b, const char *url)
{
    long i = au_history_find(&b->history, url);
    if (i == 0 && b->history.e[0].visits == 1)
        au_history_remove(&b->history, 0);
}

static void cb_url(void *ctx, AuPage *p, const char *url)
{
    AuTab *t = ctx;
    AuNavEntry *e = au_nav_current(&t->nav);
    if (e && t->loading && strcmp(e->url, url) != 0) {
        AuBrowser *b = t->win->browser;
        forget_visit(b, e->url);
        record_visit(b, url, NULL);
        menus_changed_all(b);
    }
    if (e)
        set_str(&e->url, url);
    t->win->browser->session_gen++;
    unsigned what = AU_CHANGED_TAB_INFO;
    if (is_active(t)) {
        if (!t->win->field)
            what |= AU_CHANGED_FIELD;
        /* The window title shows the address of a page without a title. */
        if (!*au_page_title(p))
            what |= AU_CHANGED_TITLE;
        what |= AU_CHANGED_SECURITY | AU_CHANGED_BOOKMARK;
    }
    changed(t->win, what);
}

/* What depends on the active tab's load: the native chrome's notices. */
#define LOAD_STATE_CHANGES                                                                                         \
    (AU_CHANGED_NAV | AU_CHANGED_STATUS | AU_CHANGED_SECURITY | AU_CHANGED_PROGRESS | AU_CHANGED_BOOKMARK)

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
    changed(w, AU_CHANGED_TAB_INFO | AU_CHANGED_PROGRESS | (is_active(t) ? LOAD_STATE_CHANGES : 0));
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
    /* The status text follows the load while no link or page text shows. */
    changed(t->win, AU_CHANGED_PROGRESS | (is_active(t) && !*t->status ? AU_CHANGED_STATUS : 0));
    (void)p;
}

static const AuPageFetcher *env_fetcher(void *ctx, const char *scheme);

static void cb_finished(void *ctx, AuPage *p, bool ok)
{
    AuTab *t = ctx;
    t->loading = false;
    loading_changed(t);
    /* The site's icon, after the document and its resources: fetched
     * unless the store has a fresh one for the host (favicon.h). */
    AuBrowser *b = t->win->browser;
    if (ok && b->site_icons && b->prefs.load_images) {
        char *icon = au_page_icon_url(p);
        if (icon) {
            const AuPageFetcher *f = env_fetcher(b, au_url_scheme_is(icon, "https") ? "https" : "http");
            au_favicons_request(b->favicons, f, au_page_url(p), icon, b->host.now ? b->host.now(b->host.ctx) : 0);
            au_free(icon);
        }
    }
}

static void cb_open(void *ctx, AuPage *p, const char *url, bool new_tab)
{
    AuTab *t = ctx;
    AuBrowserWindow *w = t->win;
    if (new_tab) {
        au_bw_open_from(w, t, url, false, tab_index(w, t) + 1);
    } else {
        int i = tab_index(w, t);
        if (i != w->active)
            au_bw_select_tab(w, i);
        au_bw_navigate(w, url);
    }
    (void)p;
}

static void apply_site_zoom(AuTab *t, const char *url);

/* ---- The document cache ---- */

/* Tells the tab's page which entry its next load is for, and which
 * entries exist (documents kept for entries that are gone are dropped). */
static void entry_load(AuTab *t)
{
    uint32_t ids[AU_NAV_MAX];
    int n = 0;
    for (int i = 0; i < t->nav.n && n < AU_NAV_MAX; i++)
        ids[n++] = t->nav.e[i].id;
    au_page_retain_entries(t->page, ids, (size_t)n);
    AuNavEntry *e = au_nav_current(&t->nav);
    au_page_set_entry(t->page, e ? e->id : 0);
}

size_t au_browser_kept_documents(const AuBrowser *b, size_t *bytes)
{
    size_t n = 0, total = 0;
    for (int i = 0; i < b->nwindows; i++) {
        for (int j = 0; j < b->windows[i]->ntabs; j++) {
            n += au_page_kept_count(b->windows[i]->tabs[j]->page);
            total += au_page_kept_bytes(b->windows[i]->tabs[j]->page);
        }
    }
    if (bytes)
        *bytes = total;
    return n;
}

bool au_browser_evict_document(AuBrowser *b)
{
    /* The document hidden longest ago, in any tab. */
    AuPage *oldest = NULL;
    uint32_t best = 0, now_ms = time_ms(b);
    for (int i = 0; i < b->nwindows; i++) {
        for (int j = 0; j < b->windows[i]->ntabs; j++) {
            uint32_t stamp;
            AuPage *p = b->windows[i]->tabs[j]->page;
            if (au_page_kept_oldest(p, &stamp) && (!oldest || now_ms - stamp > best)) {
                oldest = p;
                best = now_ms - stamp;
            }
        }
    }
    return oldest && au_page_evict_oldest(oldest);
}

void au_browser_trim_documents(AuBrowser *b)
{
    size_t budget = (size_t)b->prefs.document_cache_kb * 1024, total;
    while (au_browser_kept_documents(b, &total) && total > budget)
        if (!au_browser_evict_document(b))
            break;
}

size_t au_browser_release_documents(AuBrowser *b)
{
    size_t n = 0;
    for (int i = 0; i < b->nwindows; i++)
        for (int j = 0; j < b->windows[i]->ntabs; j++)
            n += au_page_drop_kept(b->windows[i]->tabs[j]->page);
    return n;
}

/* ---- Unloaded tabs (browser.h) ---- */

/* Whether tab i of w may give up its document now. */
static bool tab_unloadable(AuBrowserWindow *w, int i)
{
    AuBrowser *b = w->browser;
    if (i < 0 || i >= w->ntabs || i == w->active)
        return false;
    AuTab *t = w->tabs[i];
    if (t->loading || t->close_pending)
        return false;
    /* A page au_bw_idle is inside has its document on the stack. */
    if (b->idle_depth > 1 || (b->idle_depth == 1 && b->idle_page == t->page))
        return false;
    return au_page_can_discard(t->page);
}

static void unload_tab(AuBrowserWindow *w, int i)
{
    AuTab *t = w->tabs[i];
    AuNavEntry *e = au_nav_current(&t->nav);
    if (e)
        e->scroll_y = t->scroll_y;
    au_page_discard(t->page);
    set_str(&t->status, "");
    invalidate_tab(w, i);
    changed(w, AU_CHANGED_TAB_INFO);
}

bool au_bw_can_unload_tab(AuBrowserWindow *w, int i)
{
    return tab_unloadable(w, i);
}

bool au_bw_unload_tab(AuBrowserWindow *w, int i)
{
    if (!tab_unloadable(w, i))
        return false;
    unload_tab(w, i);
    return true;
}

/* The eligible tab shown longest ago, in any window; false when none. */
static bool lru_tab(AuBrowser *b, AuBrowserWindow **win, int *index, uint32_t *age_ms)
{
    uint32_t now_ms = time_ms(b), best = 0;
    bool found = false;
    for (int i = 0; i < b->nwindows; i++) {
        AuBrowserWindow *w = b->windows[i];
        for (int j = 0; j < w->ntabs; j++) {
            uint32_t age = now_ms - w->tabs[j]->shown_ms;
            if ((found && age <= best) || !tab_unloadable(w, j))
                continue;
            found = true;
            best = age;
            *win = w;
            *index = j;
        }
    }
    *age_ms = best;
    return found;
}

bool au_browser_unload_lru(AuBrowser *b)
{
    AuBrowserWindow *w;
    int i;
    uint32_t age;
    if (!lru_tab(b, &w, &i, &age))
        return false;
    unload_tab(w, i);
    b->unloaded_count++;
    return true;
}

bool au_browser_memory_pressure(AuBrowser *b, uint32_t free_kb)
{
    const AuPrefs *p = &b->prefs;
    if (!p->unload_tabs)
        return false;
    if (free_kb < (uint32_t)p->unload_free_kb)
        return au_browser_evict_document(b) || au_browser_unload_lru(b);
    if (p->unload_idle_minutes > 0 && free_kb < (uint32_t)p->unload_idle_kb) {
        AuBrowserWindow *w;
        int i;
        uint32_t age;
        if (lru_tab(b, &w, &i, &age) && age / 60000u >= (uint32_t)p->unload_idle_minutes) {
            unload_tab(w, i);
            b->unloaded_count++;
            return true;
        }
    }
    return false;
}

size_t au_browser_release_memory(AuBrowser *b)
{
    size_t n = au_browser_release_documents(b);
    if (!n && b->prefs.unload_tabs)
        while (au_browser_unload_lru(b))
            n++;
    return n;
}

/* An unloaded tab is about to be shown: its entry loads again through
 * the history cache mode, scrolled to where the tab was. */
static void load_unloaded(AuTab *t)
{
    if (!au_page_discarded(t->page))
        return;
    AuNavEntry *e = au_nav_current(&t->nav);
    const char *u = e ? e->url : au_page_url(t->page);
    char *url = au_strndup(u, strlen(u));
    int sx = t->scroll_x, sy = t->scroll_y;
    t->scroll_x = t->scroll_y = 0;
    au_page_set_scroll(t->page, 0, 0);
    entry_load(t);
    au_page_navigate_entry(t->page, url, sx, sy);
    au_free(url);
    changed(t->win, AU_CHANGED_TAB_INFO); /* no longer unloaded */
}

/* A form posted: a new entry in this tab, or a new tab. A history entry
 * for a POST result loads its URL with GET when revisited. */
static void cb_submit_post(void *ctx, AuPage *p, const char *url, const void *body, size_t len, const char *type,
                           bool new_tab)
{
    AuTab *t = ctx;
    AuBrowserWindow *w = t->win;
    (void)p;
    if (new_tab) {
        t = au_bw_open_from(w, t, "about:blank", false, tab_index(w, t) + 1);
        if (!t)
            return;
    } else {
        int i = tab_index(w, t);
        if (i != w->active)
            au_bw_select_tab(w, i);
    }
    apply_site_zoom(t, url);
    AuNavEntry *cur = au_nav_current(&t->nav);
    if (cur)
        cur->scroll_y = t->scroll_y;
    if (new_tab && t->nav.n == 1) {
        set_str(&t->nav.e[0].url, url); /* replaces the blank entry */
    } else {
        au_nav_push(&t->nav, url, "");
    }
    t->scroll_x = t->scroll_y = 0;
    au_page_set_scroll(t->page, 0, 0);
    record_visit(w->browser, url, NULL);
    w->browser->session_gen++;
    entry_load(t);
    au_page_navigate_post(t->page, url, body, len, type);
    if (is_active(t))
        changed(w, AU_CHANGED_FOCUS_PAGE | AU_CHANGED_NAV);
}

static int cb_popup(void *ctx, AuPage *p, const AuPopupItem *items, int n, int selected, AuIRect at)
{
    AuTab *t = ctx;
    AuBrowserWindow *w = t->win;
    const AuBrowserHost *h = &w->browser->host;
    (void)p;
    if (!h->popup)
        return -2;
    AuIRect pg = w->layout.page;
    AuIRect wr = {at.x0 - t->scroll_x + pg.x0, at.y0 - t->scroll_y + pg.y0, at.x1 - t->scroll_x + pg.x0,
                  at.y1 - t->scroll_y + pg.y0};
    return h->popup(h->ctx, w, items, n, selected, wr);
}

static bool cb_choose_file(void *ctx, AuPage *p, bool multiple, AuControlFile **files, uint32_t *n)
{
    AuTab *t = ctx;
    const AuBrowserHost *h = &t->win->browser->host;
    (void)p;
    return h->choose_file && h->choose_file(h->ctx, multiple, files, n);
}

static bool cb_confirm(void *ctx, AuPage *p, const char *message)
{
    AuTab *t = ctx;
    const AuBrowserHost *h = &t->win->browser->host;
    (void)p;
    return !h->confirm || h->confirm(h->ctx, t->win, message);
}

/* Go Back on the blocked-site page: the previous entry, or, in a tab
 * opened for that address alone, close the tab. */
static void cb_go_back(void *ctx, AuPage *p)
{
    AuTab *t = ctx;
    (void)p;
    if (au_nav_can_go(&t->nav, -1)) {
        if (t->win->tabs[t->win->active] == t)
            au_bw_go(t->win, -1);
    } else if (t->win->ntabs > 1) {
        t->close_pending = true;
    }
}

/* The page's load turned out to be a file to save. */
static bool cb_download(void *ctx, AuPage *p, const AuFetchResponse *r, uint32_t handoff)
{
    AuTab *t = ctx;
    AuBrowser *b = t->win->browser;
    (void)p;
    char name[128];
    if (r->filename && r->filename[0])
        snprintf(name, sizeof name, "%s", r->filename);
    else
        au_download_name_from_url(r->url, name, sizeof name);
    /* The load leaves no trace in the tab's history or the global one. */
    AuNavEntry *e = au_nav_current(&t->nav);
    if (e) {
        forget_visit(b, e->url);
        menus_changed_all(b);
    }
    if (t->nav.index > 0) {
        au_free(t->nav.e[t->nav.index].url);
        au_free(t->nav.e[t->nav.index].title);
        t->nav.n = t->nav.index;
        t->nav.index--;
    } else if (t->win->ntabs > 1) {
        t->close_pending = true; /* a tab opened just for the file */
    }
    b->session_gen++;
    if (is_active(t))
        changed(t->win, AU_CHANGED_NAV);
    au_browser_download(b, r->url, name, r->content_type, r->content_length, handoff);
    return true;
}

static void cb_status(void *ctx, AuPage *p, const char *text)
{
    AuTab *t = ctx;
    bool same = !strcmp(t->status, text ? text : "");
    set_str(&t->status, text);
    if (is_active(t)) {
        invalidate(t->win, t->win->layout.status);
        if (!same)
            changed(t->win, AU_CHANGED_STATUS);
    }
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
    if (is_active(t)) {
        if (t->win->painting)
            t->win->hscroll_dirty = true; /* not in the middle of a paint: at the next idle */
        else
            check_hscroll(t->win);
        changed(t->win, AU_CHANGED_SCROLL);
    }
    (void)p;
}

static void rehover(AuBrowserWindow *w);

static void cb_scroll_to(void *ctx, AuPage *p, int x, int y)
{
    AuTab *t = ctx;
    int ox = t->scroll_x, oy = t->scroll_y;
    /* A fragment or restored position: the page asks from inside its own
     * work (loading, script), so its pixels are not trusted to move. */
    t->scroll_x = x;
    t->scroll_y = y;
    clamp_scroll(t);
    if (t->scroll_x != ox || t->scroll_y != oy)
        t->win->browser->scroll_gen++;
    if (is_active(t) && (t->scroll_x != ox || t->scroll_y != oy)) {
        page_scrolled(t->win, t, ox, oy, false);
        changed(t->win, AU_CHANGED_SCROLL);
        rehover(t->win);
    }
    (void)p;
}

static uint32_t env_time(void *ctx)
{
    return time_ms(ctx);
}

static bool env_random(void *ctx, void *buf, size_t len)
{
    AuBrowser *b = ctx;
    return b->host.random && b->host.random(b->host.ctx, buf, len);
}

static uint32_t env_memory_free(void *ctx)
{
    AuBrowser *b = ctx;
    return b->host.memory_free_kb ? b->host.memory_free_kb(b->host.ctx) : 0;
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

/* ---- Web storage ---- */

static struct AuWebStorage *env_storage(void *ctx)
{
    return ((AuBrowser *)ctx)->storage;
}

static const struct AuHistory *env_history(void *ctx)
{
    return &((AuBrowser *)ctx)->history;
}

static const struct AuBookmarks *env_bookmarks(void *ctx)
{
    return &((AuBrowser *)ctx)->bookmarks;
}

static bool env_dark(void *ctx)
{
    return au_browser_dark((AuBrowser *)ctx);
}

/* Script changed a storage area: the documents of the same area in every
 * tab hear of it (the writer is left out by the page), and a change to a
 * tab's sessionStorage puts the session file out of date, as a scroll
 * position does. */
static void storage_changed(void *ctx, const AuStorageChange *c)
{
    AuBrowser *b = ctx;
    if (c->kind == AU_STORAGE_SESSION)
        b->scroll_gen++;
    for (int i = 0; i < b->nwindows; i++)
        for (int j = 0; j < b->windows[i]->ntabs; j++)
            au_page_storage_event(b->windows[i]->tabs[j]->page, c);
}

void au_browser_set_storage(AuBrowser *b, struct AuWebStorage *ws)
{
    if (b->own_storage)
        au_web_storage_free(b->storage);
    b->storage = ws;
    b->own_storage = false;
    if (ws)
        au_web_storage_set_listener(ws, storage_changed, b);
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
    b->favicons = au_favicons_new();
    b->storage = au_web_storage_new();
    b->own_storage = true;
    au_web_storage_set_listener(b->storage, storage_changed, b);
    return b;
}

void au_browser_free(AuBrowser *b)
{
    while (b->nwindows)
        au_browser_window_free(b->windows[b->nwindows - 1]);
    au_free(b->windows);
    au_favicons_free(b->favicons); /* cancels icon fetches while the fetchers exist */
    au_prefs_free(&b->prefs);
    au_history_free(&b->history);
    au_bookmarks_free(&b->bookmarks);
    au_downloads_free(&b->downloads);
    au_free(b->find_text);
    for (int i = 0; i < b->nclosed; i++) {
        au_nav_free(&b->closed[i]);
        if (b->storage)
            au_web_storage_ns_free(b->storage, b->closed_ns[i]);
    }
    if (b->own_storage)
        au_web_storage_free(b->storage);
    else if (b->storage)
        au_web_storage_set_listener(b->storage, NULL, NULL);
    au_free(b);
}

static void scheme_changed(AuBrowser *b);

void au_browser_prefs_changed(AuBrowser *b)
{
    b->prefs_gen++;
    scheme_changed(b);
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

bool au_browser_dark(const AuBrowser *b)
{
    switch (b->prefs.color_scheme) {
    case AU_SCHEME_DARK: return true;
    case AU_SCHEME_LIGHT: return false;
    default: return b->system_dark;
    }
}

/* The scheme reaches every tab's page (and its frames) at once; each
 * restyles at its next update, without reloading. */
static void scheme_changed(AuBrowser *b)
{
    bool dark = au_browser_dark(b);
    for (int i = 0; i < b->nwindows; i++) {
        AuBrowserWindow *w = b->windows[i];
        for (int j = 0; j < w->ntabs; j++)
            au_page_set_dark(w->tabs[j]->page, dark);
    }
}

void au_browser_set_system_dark(AuBrowser *b, bool dark)
{
    b->system_dark = dark;
    scheme_changed(b);
}

void au_browser_bookmarks_changed(AuBrowser *b)
{
    for (int i = 0; i < b->nwindows; i++) {
        invalidate(b->windows[i], b->windows[i]->layout.bookmark);
        changed(b->windows[i], AU_CHANGED_MENUS | AU_CHANGED_BOOKMARK);
    }
}

void au_browser_history_changed(AuBrowser *b)
{
    menus_changed_all(b);
}

bool au_browser_busy(const AuBrowser *b)
{
    for (int i = 0; i < b->nwindows; i++) {
        if (b->windows[i]->rehover_pending)
            return true;
    }
    for (int i = 0; i < b->nwindows; i++)
        for (int j = 0; j < b->windows[i]->ntabs; j++)
            if (au_page_busy(b->windows[i]->tabs[j]->page))
                return true;
    return false;
}

static AuBrowserWindow *window_new(AuBrowser *b, int width, int height, bool native_chrome);

AuBrowserWindow *au_browser_window_new(AuBrowser *b, int width, int height)
{
    return window_new(b, width, height, false);
}

AuBrowserWindow *au_browser_window_new_native(AuBrowser *b, int width, int height)
{
    return window_new(b, width, height, true);
}

static AuBrowserWindow *window_new(AuBrowser *b, int width, int height, bool native_chrome)
{
    AuBrowserWindow *w = au_calloc(sizeof *w);
    w->native_chrome = native_chrome;
    w->browser = b;
    w->width = width;
    w->height = height;
    w->active = -1;
    w->drag_tab = -1;
    w->x = w->y = -1;
    w->hover.part = w->pressed.part = AU_HIT_NONE;
    w->active_window = true;
    b->windows = au_realloc(b->windows, (size_t)(b->nwindows + 1) * sizeof *b->windows);
    b->windows[b->nwindows++] = w;
    if (native_chrome)
        au_chrome_layout_page_only(&w->layout, width, height, 0);
    else
        au_chrome_layout(&w->layout, width, height, b->prefs.tab_position, b->prefs.tab_column_width, 0, 0);
    b->session_gen++;
    return w;
}

static void free_tab(AuTab *t)
{
    au_page_free(t->page);
    /* A closed tab that can be reopened gave its namespace to the closed
     * list (push_closed) and has none here. */
    if (t->storage_ns && t->win->browser->storage)
        au_web_storage_ns_free(t->win->browser->storage, t->storage_ns);
    au_nav_free(&t->nav);
    au_free(t->status);
    au_free(t);
}

void au_browser_window_free(AuBrowserWindow *w)
{
    AuBrowser *b = w->browser;
    /* Out of the browser's lists first: freeing a page runs its unload
     * handlers, whose script may write localStorage, and the storage event
     * then goes to every listed tab (storage_changed). A tab freed earlier
     * in this loop must not be among them, nor this window's others. */
    for (int i = 0; i < b->nwindows; i++) {
        if (b->windows[i] == w) {
            memmove(&b->windows[i], &b->windows[i + 1], (size_t)(b->nwindows - i - 1) * sizeof *b->windows);
            b->nwindows--;
            break;
        }
    }
    au_bench_free(w->bench);
    w->bench = NULL;
    AuTab **tabs = w->tabs;
    int ntabs = w->ntabs;
    w->tabs = NULL;
    w->ntabs = 0;
    w->active = -1;
    for (int i = 0; i < ntabs; i++)
        free_tab(tabs[i]);
    au_free(tabs);
    au_free(w->field);
    if (w->label_list)
        au_fontlist_free(w->label_list);
    if (w->small_list)
        au_fontlist_free(w->small_list);
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
    t->shown_ms = time_ms(b);
    au_nav_init(&t->nav);
    AuPageEnv env = {b->fonts, &b->prefs, b, env_time, env_read, env_fetcher, b->highlight,
                     b->host.random ? env_random : NULL, b->host.coro, env_memory_free, env_storage,
                     env_history, env_bookmarks, env_dark};
    AuPageCallbacks cb = {t,         cb_title,  cb_url,        cb_started,      cb_progress,  cb_finished,
                          cb_open,   cb_status, cb_invalidate, cb_content_size, cb_scroll_to, cb_download,
                          cb_submit_post, cb_popup, cb_choose_file, cb_confirm, cb_go_back};
    t->page = au_page_new(&env, &cb);
    if (b->storage) {
        t->storage_ns = au_web_storage_ns_new(b->storage);
        au_page_set_storage_ns(t->page, t->storage_ns);
    }
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
    changed(w, AU_CHANGED_TABS);
    return t;
}

/* ---- Selection and find ---- */

bool au_bw_has_selection(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    return t && au_page_has_selection(t->page);
}

bool au_bw_selection_text(AuBrowserWindow *w, AuBuf *out)
{
    AuTab *t = au_bw_active_tab(w);
    return t && au_page_selection_text(t->page, out);
}

void au_bw_select_all(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    if (t)
        au_page_select_all(t->page);
}

bool au_bw_find(AuBrowserWindow *w, const char *text, bool case_sensitive)
{
    AuBrowser *b = w->browser;
    if (!text || !*text)
        return false;
    au_prefs_set_str(&b->find_text, text);
    b->find_case = case_sensitive;
    return au_bw_find_again(w, true);
}

bool au_bw_find_again(AuBrowserWindow *w, bool forward)
{
    AuTab *t = au_bw_active_tab(w);
    AuBrowser *b = w->browser;
    return t && b->find_text && au_page_find(t->page, b->find_text, forward, b->find_case);
}

/* ---- Text zoom ----
 * Each tab has a text zoom; the last zoom chosen for a site (host, or
 * the scheme for URLs without one) is kept in prefs.zoom_sites as
 * "key=percent" items separated by spaces, and applied when a tab moves
 * to that site. */

static const float zoom_steps[] = {0.5f, 0.67f, 0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};
#define NZOOM ((int)(sizeof zoom_steps / sizeof zoom_steps[0]))
#define MAX_ZOOM_SITES 64

static void zoom_key(const char *url, char *out, size_t cap)
{
    if (!strncmp(url, "view-source:", 12))
        url += 12;
    au_url_host(url, out, cap);
    if (!out[0]) {
        size_t n = au_url_scheme_len(url, strlen(url));
        if (n + 2 > cap)
            n = 0;
        memcpy(out, url, n);
        out[n] = ':';
        out[n + 1] = 0;
    }
    for (char *p = out; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p + 32);
}

float au_browser_site_zoom(const AuBrowser *b, const char *url)
{
    char key[256];
    zoom_key(url, key, sizeof key);
    size_t kn = strlen(key);
    const char *s = b->prefs.zoom_sites ? b->prefs.zoom_sites : "";
    while (*s) {
        while (*s == ' ')
            s++;
        const char *e = s;
        while (*e && *e != ' ')
            e++;
        if ((size_t)(e - s) > kn + 1 && !memcmp(s, key, kn) && s[kn] == '=') {
            int pct = atoi(s + kn + 1);
            if (pct >= 50 && pct <= 300)
                return (float)pct / 100.0f;
        }
        s = e;
    }
    return 1.0f;
}

static void set_site_zoom(AuBrowser *b, const char *url, float zoom)
{
    char key[256];
    zoom_key(url, key, sizeof key);
    size_t kn = strlen(key);
    AuBuf out;
    au_buf_init(&out);
    int pct = (int)(zoom * 100.0f + 0.5f);
    if (pct != 100 && kn && !strchr(key, ' ') && !strchr(key, '='))
        au_buf_printf(&out, "%s=%d", key, pct);
    int kept = 1;
    const char *s = b->prefs.zoom_sites ? b->prefs.zoom_sites : "";
    while (*s && kept < MAX_ZOOM_SITES) {
        while (*s == ' ')
            s++;
        const char *e = s;
        while (*e && *e != ' ')
            e++;
        if (e > s && !((size_t)(e - s) > kn && !memcmp(s, key, kn) && s[kn] == '=')) {
            if (out.len)
                au_buf_putc(&out, ' ');
            au_buf_append(&out, s, (size_t)(e - s));
            kept++;
        }
        s = e;
    }
    au_prefs_set_str(&b->prefs.zoom_sites, au_buf_cstr(&out));
    au_buf_free(&out);
    b->prefs_gen++;
}

/* A tab moving to url takes the site's zoom when the site changes. */
static void apply_site_zoom(AuTab *t, const char *url)
{
    char a[256], c[256];
    zoom_key(url, a, sizeof a);
    zoom_key(au_page_url(t->page), c, sizeof c);
    if (strcmp(a, c) != 0 || !au_nav_current(&t->nav))
        au_page_set_zoom(t->page, au_browser_site_zoom(t->win->browser, url));
}

void au_bw_zoom(AuBrowserWindow *w, int dir)
{
    AuTab *t = au_bw_active_tab(w);
    if (!t)
        return;
    float z = au_page_zoom(t->page), nz = 1.0f;
    if (dir > 0) {
        nz = zoom_steps[NZOOM - 1];
        for (int i = 0; i < NZOOM; i++)
            if (zoom_steps[i] > z + 0.001f) {
                nz = zoom_steps[i];
                break;
            }
    } else if (dir < 0) {
        nz = zoom_steps[0];
        for (int i = NZOOM - 1; i >= 0; i--)
            if (zoom_steps[i] < z - 0.001f) {
                nz = zoom_steps[i];
                break;
            }
    }
    if (nz == z)
        return;
    au_page_set_zoom(t->page, nz);
    set_site_zoom(w->browser, au_page_url(t->page), nz);
    clamp_scroll(t);
    invalidate(w, w->layout.page);
    changed(w, AU_CHANGED_SCROLL);
}

bool au_bw_can_zoom(AuBrowserWindow *w, int dir)
{
    AuTab *t = au_bw_active_tab(w);
    if (!t)
        return false;
    float z = au_page_zoom(t->page);
    if (dir > 0)
        return z < zoom_steps[NZOOM - 1] - 0.001f;
    if (dir < 0)
        return z > zoom_steps[0] + 0.001f;
    return z < 0.999f || z > 1.001f;
}

static bool fresh_blank(AuTab *t);
static bool hand_off(AuBrowser *b, const char *url);
static void push_closed(AuBrowser *b, AuTab *t);

/* Loads url into tab t as a new back/forward entry. */
static void load_new(AuTab *t, const char *url)
{
    apply_site_zoom(t, url);
    AuNavEntry *cur = au_nav_current(&t->nav);
    if (cur)
        cur->scroll_y = t->scroll_y;
    au_nav_push(&t->nav, url, "");
    t->scroll_x = t->scroll_y = 0;
    au_page_set_scroll(t->page, 0, 0);
    record_visit(t->win->browser, url, NULL);
    t->win->browser->session_gen++;
    entry_load(t);
    if (is_active(t))
        changed(t->win, AU_CHANGED_NAV); /* the entry pushed: Back is on, Forward off */
    au_page_navigate(t->page, url);
}

AuTab *au_bw_new_tab(AuBrowserWindow *w, const char *url, bool activate, int index)
{
    return au_bw_open_from(w, NULL, url, activate, index);
}

AuTab *au_bw_open_from(AuBrowserWindow *w, AuTab *opener, const char *url, bool activate, int index)
{
    if (w->ntabs > 0 && hand_off(w->browser, url))
        return NULL;
    if (index < 0)
        index = w->active >= 0 ? w->active + 1 : w->ntabs;
    AuTab *t = make_tab(w, index);
    if (opener && w->browser->storage)
        au_web_storage_ns_copy(w->browser->storage, opener->storage_ns, t->storage_ns);
    relayout(w, false);
    if (activate || w->active < 0)
        au_bw_select_tab(w, tab_index(w, t));
    load_new(t, url && *url ? url : "about:blank");
    changed(w, AU_CHANGED_MENUS);
    if (au_bw_active_tab(w) == t)
        changed(w, fresh_blank(t) ? AU_CHANGED_FOCUS_FIELD : AU_CHANGED_FOCUS_PAGE);
    return t;
}

/* ---- Keyboard focus ----
 * The rule: after a navigation or a tab switch the page has the focus,
 * unless the user is typing in the address field (it has the focus and
 * holds an edit), and except that a tab showing a fresh blank page puts
 * the focus in the field, ready for an address. The shell moves the
 * focus when it gets AU_CHANGED_FOCUS_PAGE or AU_CHANGED_FOCUS_FIELD. */

bool au_bw_typing(const AuBrowserWindow *w)
{
    return w->field_focused && w->field != NULL;
}

static bool fresh_blank(AuTab *t)
{
    return t->nav.n <= 1 && !strcmp(au_page_url(t->page), "about:blank");
}

static void focus_after_navigation(AuBrowserWindow *w)
{
    if (!au_bw_typing(w))
        changed(w, AU_CHANGED_FOCUS_PAGE);
}

void au_bw_select_tab(AuBrowserWindow *w, int i)
{
    if (i < 0 || i >= w->ntabs)
        return;
    int old = w->active;
    uint32_t t_now = time_ms(w->browser);
    if (old >= 0 && old < w->ntabs)
        w->tabs[old]->shown_ms = t_now; /* shown until now */
    w->active = i;
    au_free(w->field);
    w->field = NULL;
    AuTab *t = w->tabs[i];
    t->shown_ms = t_now;
    load_unloaded(t);
    changed(w, fresh_blank(t) ? AU_CHANGED_FOCUS_FIELD : AU_CHANGED_FOCUS_PAGE);
    set_viewport(t);
    check_hscroll(w);
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
    changed(w, AU_CHANGED_FIELD | AU_CHANGED_TITLE | AU_CHANGED_SCROLL | AU_CHANGED_MENUS | LOAD_STATE_CHANGES |
                   (old != i ? AU_CHANGED_ACTIVE_TAB : 0));
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
    push_closed(w->browser, t);
    free_tab(t);
    int next = w->active;
    if (i < w->active)
        next = w->active - 1;
    else if (i == w->active)
        next = i < w->ntabs ? i : w->ntabs - 1; /* the tab to the right, else left */
    w->active = -1;
    w->hover.part = w->pressed.part = AU_HIT_NONE;
    changed(w, AU_CHANGED_TABS);
    relayout(w, true);
    au_bw_select_tab(w, next);
    w->browser->session_gen++;
}

AuTab *au_bw_duplicate_tab(AuBrowserWindow *w, int i)
{
    if (i < 0 || i >= w->ntabs)
        return NULL;
    AuTab *src = w->tabs[i];
    AuTab *t = make_tab(w, i + 1);
    if (w->browser->storage)
        au_web_storage_ns_copy(w->browser->storage, src->storage_ns, t->storage_ns);
    for (int k = 0; k < src->nav.n; k++) {
        au_nav_push(&t->nav, src->nav.e[k].url, src->nav.e[k].title);
        t->nav.e[k].scroll_y = src->nav.e[k].scroll_y;
    }
    t->nav.index = src->nav.index;
    relayout(w, false);
    au_bw_select_tab(w, i + 1);
    AuNavEntry *e = au_nav_current(&t->nav);
    char *url = au_strndup(e ? e->url : "about:blank", strlen(e ? e->url : "about:blank"));
    au_page_set_zoom(t->page, au_page_zoom(src->page));
    entry_load(t);
    au_page_navigate_entry(t->page, url, src->scroll_x, src->scroll_y);
    au_free(url);
    changed(w, AU_CHANGED_MENUS | AU_CHANGED_FOCUS_PAGE);
    return t;
}

/* A new tab after the active one holding a copy of nav, at entry index,
 * loaded and scrolled to where that entry was left; activated or not.
 * Its sessionStorage starts as a copy of namespace ns_from (0: empty). */
static AuTab *tab_from_nav(AuBrowserWindow *w, const AuNavList *nav, int index, int at, bool activate,
                           uint32_t ns_from)
{
    if (index < 0 || index >= nav->n)
        return NULL;
    AuTab *t = make_tab(w, at);
    if (ns_from && w->browser->storage)
        au_web_storage_ns_copy(w->browser->storage, ns_from, t->storage_ns);
    for (int k = 0; k < nav->n; k++) {
        au_nav_push(&t->nav, nav->e[k].url, nav->e[k].title);
        t->nav.e[t->nav.n - 1].scroll_y = nav->e[k].scroll_y;
    }
    t->nav.index = index;
    relayout(w, false);
    if (activate)
        au_bw_select_tab(w, tab_index(w, t));
    AuNavEntry *e = &t->nav.e[index];
    char *url = au_strndup(e->url, strlen(e->url));
    apply_site_zoom(t, url);
    entry_load(t);
    au_page_navigate_entry(t->page, url, 0, e->scroll_y);
    au_free(url);
    changed(w, AU_CHANGED_MENUS);
    return t;
}

AuTab *au_bw_go_new_tab(AuBrowserWindow *w, int delta)
{
    AuTab *src = au_bw_active_tab(w);
    if (!src || !au_nav_can_go(&src->nav, delta))
        return NULL;
    /* The current entry's position is the tab's own. */
    AuNavEntry *cur = au_nav_current(&src->nav);
    if (cur)
        cur->scroll_y = src->scroll_y;
    return tab_from_nav(w, &src->nav, src->nav.index + delta, w->active + 1, false, src->storage_ns);
}

int au_bw_history_items(AuBrowserWindow *w, int dir, AuHistoryItem *out, int cap)
{
    AuTab *t = au_bw_active_tab(w);
    int n = 0;
    if (!t || !dir)
        return 0;
    for (int i = t->nav.index + (dir < 0 ? -1 : 1); i >= 0 && i < t->nav.n && n < cap; i += dir < 0 ? -1 : 1) {
        out[n].delta = i - t->nav.index;
        out[n].url = t->nav.e[i].url;
        out[n].title = t->nav.e[i].title[0] ? t->nav.e[i].title : t->nav.e[i].url;
        n++;
    }
    return n;
}

/* ---- Recently closed tabs ---- */

static void push_closed(AuBrowser *b, AuTab *t)
{
    if (!t->nav.n)
        return;
    AuNavEntry *cur = au_nav_current(&t->nav);
    if (cur)
        cur->scroll_y = t->scroll_y;
    if (b->nclosed == AU_MAX_CLOSED_TABS) {
        au_nav_free(&b->closed[0]);
        if (b->storage)
            au_web_storage_ns_free(b->storage, b->closed_ns[0]);
        memmove(&b->closed[0], &b->closed[1], (AU_MAX_CLOSED_TABS - 1) * sizeof b->closed[0]);
        memmove(&b->closed_ns[0], &b->closed_ns[1], (AU_MAX_CLOSED_TABS - 1) * sizeof b->closed_ns[0]);
        b->nclosed--;
    }
    /* Move the list and the sessionStorage: the tab is about to be freed
     * (the page first, so what its documents write as they go is kept). */
    b->closed_ns[b->nclosed] = t->storage_ns;
    t->storage_ns = 0;
    b->closed[b->nclosed++] = t->nav;
    au_nav_init(&t->nav);
}

bool au_browser_can_reopen(const AuBrowser *b)
{
    return b->nclosed > 0;
}

AuTab *au_bw_reopen_closed_tab(AuBrowserWindow *w)
{
    AuBrowser *b = w->browser;
    if (!b->nclosed)
        return NULL;
    AuNavList nav = b->closed[--b->nclosed];
    uint32_t ns = b->closed_ns[b->nclosed];
    AuTab *t = tab_from_nav(w, &nav, nav.index, w->active + 1, true, ns);
    au_nav_free(&nav);
    if (b->storage)
        au_web_storage_ns_free(b->storage, ns);
    if (t)
        changed(w, AU_CHANGED_FOCUS_PAGE);
    return t;
}

void au_bw_close_other_tabs(AuBrowserWindow *w, int keep)
{
    if (keep < 0 || keep >= w->ntabs)
        return;
    AuTab *k = w->tabs[keep];
    au_bw_select_tab(w, keep);
    for (int i = w->ntabs - 1; i >= 0; i--)
        if (w->tabs[i] != k)
            au_bw_close_tab(w, i);
}

void au_bw_reload_tab(AuBrowserWindow *w, int i)
{
    if (i >= 0 && i < w->ntabs)
        au_page_reload(w->tabs[i]->page);
}

bool au_bw_can_view_source(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    if (!t)
        return false;
    const char *u = au_page_url(t->page);
    return au_url_scheme_is(u, "file") || au_url_scheme_is(u, "http") || au_url_scheme_is(u, "https");
}

/* A tab whose page is filled by the caller (not loaded from url). */
static AuTab *new_tab_for(AuBrowserWindow *w, const char *url)
{
    AuTab *t = make_tab(w, w->active >= 0 ? w->active + 1 : w->ntabs);
    relayout(w, false);
    au_bw_select_tab(w, tab_index(w, t));
    au_nav_push(&t->nav, url, "");
    entry_load(t);
    w->browser->session_gen++;
    changed(w, AU_CHANGED_MENUS | AU_CHANGED_FOCUS_PAGE);
    return t;
}

void au_bw_view_source(AuBrowserWindow *w)
{
    if (!au_bw_can_view_source(w))
        return;
    AuTab *cur = au_bw_active_tab(w);
    AuBuf u;
    au_buf_init(&u);
    au_buf_puts(&u, "view-source:");
    au_buf_puts(&u, au_page_url(cur->page));
    const char *data;
    size_t len;
    const char *pu = au_page_url(cur->page);
    if ((au_url_scheme_is(pu, "http") || au_url_scheme_is(pu, "https")) && au_page_source(cur->page, &data, &len)) {
        /* The bytes kept for Save Page As: nothing is fetched again. */
        AuTab *t = new_tab_for(w, au_buf_cstr(&u));
        au_page_navigate_source(t->page, au_buf_cstr(&u), data, len);
    } else {
        au_bw_new_tab(w, au_buf_cstr(&u), true, w->active + 1);
    }
    au_buf_free(&u);
}

bool au_bw_show_certificate(AuBrowserWindow *w)
{
    AuTab *cur = au_bw_active_tab(w);
    if (!cur)
        return false;
    AuBuf html;
    au_buf_init(&html);
    bool ok = au_page_certificate_html(cur->page, &html);
    if (ok) {
        AuTab *t = new_tab_for(w, "about:certificate");
        au_page_load_internal(t->page, "about:certificate", html.data, html.len);
    }
    au_buf_free(&html);
    return ok;
}

AuDownload *au_browser_download(AuBrowser *b, const char *url, const char *name, const char *mime, int64_t total,
                                uint32_t handoff)
{
    char clean[128];
    au_hfs_sanitize(name && *name ? name : "download", NULL, NULL, clean, sizeof clean);
    if (b->host.confirm_download && !b->host.confirm_download(b->host.ctx, url, clean, mime ? mime : "")) {
        if (handoff && b->downloads.src.discard) {
            AuDownload tmp;
            memset(&tmp, 0, sizeof tmp);
            tmp.handoff = handoff;
            tmp.url = (char *)url;
            tmp.filename = (char *)"";
            b->downloads.src.discard(b->downloads.src.ctx, &tmp);
        }
        return NULL;
    }
    AuDownload *d = au_downloads_add(&b->downloads, url, clean, mime ? mime : "", total, now(b));
    d->handoff = handoff;
    if (b->host.download_added)
        b->host.download_added(b->host.ctx, d);
    return d;
}

AuDownload *au_bw_save_url(AuBrowserWindow *w, const char *url)
{
    if (!url || !(au_url_scheme_is(url, "http") || au_url_scheme_is(url, "https")))
        return NULL;
    char name[128];
    au_download_name_from_url(url, name, sizeof name);
    return au_browser_download(w->browser, url, name, "", -1, 0);
}

bool au_bw_has_page_source(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    return t && au_page_has_source(t->page);
}

bool au_bw_page_source(AuBrowserWindow *w, const char **data, size_t *len)
{
    AuTab *t = au_bw_active_tab(w);
    return t && au_page_source(t->page, data, len);
}

void au_bw_save_name(AuBrowserWindow *w, char *out, size_t cap)
{
    AuTab *t = au_bw_active_tab(w);
    char name[256], host[256];
    name[0] = host[0] = 0;
    if (t) {
        const char *u = au_page_url(t->page);
        if (!strncmp(u, "view-source:", 12))
            u += 12;
        au_url_host(u, host, sizeof host);
        au_download_name_from_url(u, name, sizeof name);
        /* A directory URL names its host: use the title instead. */
        const char *title = au_page_title(t->page);
        if ((!name[0] || !strcmp(name, host)) && *title)
            snprintf(name, sizeof name, "%s", title);
    }
    if (!name[0])
        snprintf(name, sizeof name, "%s", "page");
    if (!strchr(name, '.') || !strcmp(name, host)) {
        size_t n = strlen(name);
        if (n + 6 < sizeof name)
            memcpy(name + n, ".html", 6);
    }
    au_hfs_sanitize(name, NULL, NULL, out, cap);
}

AuBookmark *au_browser_bookmark_url(AuBrowser *b, const char *url, const char *title)
{
    AuBookmark *have = au_bookmark_find_url(&b->bookmarks, url);
    if (have)
        return have;
    AuBookmark *made = au_bookmark_add(&b->bookmarks, NULL, NULL, title && *title ? title : url, url, now(b));
    au_browser_bookmarks_changed(b);
    return made;
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
    changed(w, AU_CHANGED_MENUS | AU_CHANGED_TABS);
}

/* ---- Navigation ---- */

bool au_browser_opens_url(const AuBrowser *b, const char *url)
{
    size_t n = au_url_scheme_len(url, strlen(url));
    if (!n)
        return true;
    static const char *const own[] = {"about", "file", "view-source", "http", "https"};
    for (size_t i = 0; i < sizeof own / sizeof own[0]; i++)
        if (au_url_scheme_is(url, own[i]))
            return true;
    for (int i = 0; i < b->nfetchers; i++)
        if (strlen(b->fetchers[i].scheme) == n && !strncmp(b->fetchers[i].scheme, url, n))
            return true;
    return false;
}

/* An address Aura does not open itself goes to the host (a helper
 * application); true when the host took it. */
static bool hand_off(AuBrowser *b, const char *url)
{
    return url && b->host.open_external && !au_browser_opens_url(b, url) &&
           b->host.open_external(b->host.ctx, url);
}

void au_bw_navigate(AuBrowserWindow *w, const char *url)
{
    AuTab *t = au_bw_active_tab(w);
    if (t && !strncmp(url, AU_BENCH_PREFIX, strlen(AU_BENCH_PREFIX))) {
        /* The page benchmark: it drives the tab itself. */
        au_bench_free(w->bench);
        w->bench = NULL;
        au_free(w->field);
        w->field = NULL;
        w->bench = au_bench_start(w, url + strlen(AU_BENCH_PREFIX));
        changed(w, AU_CHANGED_FIELD | AU_CHANGED_TAB_INFO);
        return;
    }
    if (!t || hand_off(w->browser, url))
        return;
    bool typing = au_bw_typing(w);
    if (!typing) {
        au_free(w->field);
        w->field = NULL;
    }
    load_new(t, url);
    invalidate(w, w->layout.toolbar);
    changed(w, AU_CHANGED_FIELD | AU_CHANGED_SCROLL | AU_CHANGED_MENUS | AU_CHANGED_SECURITY | AU_CHANGED_BOOKMARK |
                   AU_CHANGED_TAB_INFO);
    focus_after_navigation(w);
}

void au_bw_navigate_input(AuBrowserWindow *w, const char *text)
{
    char *url = au_url_from_input(text, w->browser->prefs.search_template, NULL);
    if (!url)
        return;
    if (au_bw_active_tab(w) && hand_off(w->browser, url)) {
        /* A helper application took it: the field shows the page's
         * address again and the page keeps the focus. */
        au_free(w->field);
        w->field = NULL;
        invalidate(w, w->layout.toolbar);
        changed(w, AU_CHANGED_FIELD | AU_CHANGED_FOCUS_PAGE);
    } else {
        au_bw_navigate(w, url);
    }
    au_free(url);
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
    if (!au_bw_typing(w)) {
        au_free(w->field);
        w->field = NULL;
    }
    record_visit(w->browser, url, e->title);
    /* The page scrolls back to sy once it is loaded far enough. */
    t->scroll_x = t->scroll_y = 0;
    au_page_set_scroll(t->page, 0, 0);
    apply_site_zoom(t, url);
    entry_load(t);
    au_page_navigate_entry(t->page, url, 0, sy);
    au_free(url);
    clamp_scroll(t);
    w->browser->session_gen++;
    invalidate(w, w->layout.toolbar);
    invalidate(w, w->layout.page);
    changed(w, AU_CHANGED_FIELD | AU_CHANGED_SCROLL | AU_CHANGED_MENUS | AU_CHANGED_NAV | AU_CHANGED_SECURITY |
                   AU_CHANGED_BOOKMARK | AU_CHANGED_TAB_INFO);
    focus_after_navigation(w);
}

void au_bw_back(AuBrowserWindow *w) { au_bw_go(w, -1); }
void au_bw_forward(AuBrowserWindow *w) { au_bw_go(w, 1); }

/* A reload of a POST result sends the form again: ask first. */
static bool confirm_repost(AuBrowserWindow *w, AuTab *t)
{
    const AuBrowserHost *h = &w->browser->host;
    return !au_page_is_post(t->page) || !h->confirm ||
           h->confirm(h->ctx, w, "This page was the answer to a form. Reloading it sends the form again. Send it again?");
}

void au_bw_reload(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    if (t && confirm_repost(w, t))
        au_page_reload(t->page);
}

void au_bw_reload_bypass(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    if (t && confirm_repost(w, t))
        au_page_reload_bypass(t->page);
}

/* ---- Form fields ---- */

bool au_bw_page_editing(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    return t && !w->field_focused && au_page_editing(t->page);
}

bool au_bw_text_field_at(AuBrowserWindow *w, int x, int y)
{
    AuTab *t = au_bw_active_tab(w);
    AuIRect pg = w->layout.page;
    if (!t || x < pg.x0 || y < pg.y0 || x >= pg.x1 || y >= pg.y1)
        return false;
    return au_page_text_field_at(t->page, x - pg.x0 + t->scroll_x, y - pg.y0 + t->scroll_y);
}

bool au_bw_page_can_edit(AuBrowserWindow *w, int cmd)
{
    AuTab *t = au_bw_active_tab(w);
    return t && !w->field_focused && au_page_can_edit(t->page, cmd);
}

bool au_bw_page_edit(AuBrowserWindow *w, int cmd, const char *text, size_t len, AuBuf *out)
{
    AuTab *t = au_bw_active_tab(w);
    return t && !w->field_focused && au_page_edit(t->page, cmd, text, len, out);
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
    AuTab *t = au_bw_active_tab(w);
    if (focused && t)
        au_page_blur(t->page); /* the address field took the keyboard */
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

bool au_complete_fill(const char *typed, const char *url, AuBuf *out)
{
    if (!typed || !url)
        return false;
    size_t tn = strlen(typed);
    if (!tn)
        return false;
    const char *key = url;
    size_t kn = strlen(url);
    if (au_url_scheme_len(typed, tn) == 0) {
        size_t k = au_url_scheme_len(url, kn);
        key = url + (k ? k + 1 : 0);
        if (!strncmp(key, "//", 2))
            key += 2;
        if (strncmp(typed, "www.", 4) != 0 && !strncmp(key, "www.", 4))
            key += 4;
        kn = strlen(key);
    }
    if (kn <= tn || !au_ieq(key, tn, typed, tn))
        return false;
    au_buf_puts(out, typed);
    au_buf_puts(out, key + tn);
    return true;
}

bool au_bw_complete_field(AuBrowserWindow *w, const char *typed, AuBuf *out)
{
    if (!typed || !*typed)
        return false;
    return au_complete_fill(typed, au_bw_complete(w, typed), out);
}

/* ---- Geometry and painting ---- */

void au_bw_resize(AuBrowserWindow *w, int width, int height)
{
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;
    if (w->width != width || w->height != height)
        w->browser->session_gen++;
    w->width = width;
    w->height = height;
    relayout(w, true);
}

void au_bw_set_position(AuBrowserWindow *w, int x, int y)
{
    if (w->x == x && w->y == y)
        return;
    w->x = x;
    w->y = y;
    w->browser->session_gen++;
}

void au_bw_set_active(AuBrowserWindow *w, bool active)
{
    if (w->active_window == active)
        return;
    w->active_window = active;
    w->hover.part = AU_HIT_NONE;
    invalidate(w, w->layout.tabstrip);
}

/* ---- What the chrome shows ---- */

int au_bw_tab_count(const AuBrowserWindow *w)
{
    return w->ntabs;
}

int au_bw_active_index(const AuBrowserWindow *w)
{
    return w->active >= 0 && w->active < w->ntabs ? w->active : -1;
}

bool au_bw_tab_info(AuBrowserWindow *w, int i, AuChromeTab *out)
{
    memset(out, 0, sizeof *out);
    if (i < 0 || i >= w->ntabs)
        return false;
    AuTab *x = w->tabs[i];
    const char *u = au_page_url(x->page);
    bool blank = !strcmp(u, "about:blank");
    const char *title = au_page_title(x->page);
    if (!*title)
        title = blank ? "New Tab" : u;
    out->title = title;
    out->url = blank ? "" : u;
    out->loading = x->loading;
    out->unloaded = au_page_discarded(x->page);
    out->icon = au_favicons_for_url(w->browser->favicons, u);
    if (x->loading) {
        float p = au_page_progress(x->page);
        out->progress = p < 0 ? 0 : p > 1 ? 1 : p;
    }
    return true;
}

bool au_bw_can_go(AuBrowserWindow *w, int delta)
{
    AuTab *t = au_bw_active_tab(w);
    return t && delta && au_nav_can_go(&t->nav, delta);
}

bool au_bw_loading(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    return t && t->loading;
}

float au_bw_progress(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    if (!t || !t->loading)
        return 0;
    float p = au_page_progress(t->page);
    return p < 0 ? 0 : p > 1 ? 1 : p;
}

AuSecurity au_bw_security(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    return t ? au_page_security(t->page) : AU_SECURITY_NONE;
}

void au_bw_status_text(AuBrowserWindow *w, char *out, size_t cap)
{
    if (!cap)
        return;
    out[0] = 0;
    AuTab *t = au_bw_active_tab(w);
    if (!t)
        return;
    if (t->loading && !*t->status) {
        au_page_status_text(t->page, out, cap);
        return;
    }
    /* Cut at a character boundary when it does not fit. */
    size_t n = strlen(t->status);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)t->status[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(out, t->status, n);
    out[n] = 0;
}

void au_bw_progress_text(AuBrowserWindow *w, char *out, size_t cap)
{
    if (!cap)
        return;
    out[0] = 0;
    AuTab *t = au_bw_active_tab(w);
    if (t && t->loading) {
        float p = au_page_progress(t->page);
        p = p < 0 ? 0 : p > 1 ? 1 : p;
        snprintf(out, cap, "Loading, %d%%", (int)(p * 100.0f + 0.5f));
    }
}

int au_bw_blocked_count(AuBrowserWindow *w)
{
    AuTab *t = au_bw_active_tab(w);
    if (!t)
        return 0;
    AuPageStats ps;
    au_page_stats(t->page, &ps);
    return (int)ps.blocked;
}

void au_bw_scroll_pos(AuBrowserWindow *w, int *x, int *y)
{
    AuTab *t = au_bw_active_tab(w);
    *x = t ? t->scroll_x : 0;
    *y = t ? t->scroll_y : 0;
}

/* Paints the Platinum chrome from the same accessors a native port
 * reads. */
static void paint_chrome(AuBrowserWindow *w, AuCanvas *c, AuIRect clip)
{
    AuTab *t = au_bw_active_tab(w);
    AuChromeTab stack_tabs[16];
    AuChromeTab *tabs = w->ntabs <= 16 ? stack_tabs : au_alloc((size_t)w->ntabs * sizeof *tabs);
    for (int i = 0; i < w->ntabs; i++)
        au_bw_tab_info(w, i, &tabs[i]);
    AuChromeState s;
    memset(&s, 0, sizeof s);
    s.tabs = tabs;
    s.active = w->active;
    s.hover = w->hover;
    s.pressed = w->pressed;
    s.dragging = w->drag_tab;
    s.can_back = au_bw_can_go(w, -1);
    s.can_forward = au_bw_can_go(w, 1);
    s.loading = au_bw_loading(w);
    s.bookmarked = au_bw_is_bookmarked(w);
    s.security = au_bw_security(w);
    s.field_text = au_bw_field_text(w);
    s.field_focused = w->field_focused;
    s.draw_field_text = !w->native_field;
    char status[300];
    au_bw_status_text(w, status, sizeof status);
    s.status_text = t && !(t->loading && !*t->status) ? t->status : status; /* uncut */
    s.progress = t ? au_page_progress(t->page) : 0;
    char progress[32];
    au_bw_progress_text(w, progress, sizeof progress);
    s.progress_text = progress;
    s.blocked = au_bw_blocked_count(w);
    s.throbber_frame = w->throbber_frame;
    s.draw_native_standins = w->native_standins;
    s.active_window = w->active_window;
    if (!w->label_list && w->browser->fonts) {
        w->label_list = au_fontlist_parse(w->browser->fonts, AU_CHROME_LABEL_FAMILIES, 400, false);
        w->small_list = au_fontlist_parse(w->browser->fonts, AU_CHROME_SMALL_FAMILIES, 400, false);
    }
    s.label_list = w->label_list;
    s.small_list = w->small_list;
    au_chrome_paint(c, w->browser->fonts, &w->layout, &s, clip);
    if (tabs != stack_tabs)
        au_free(tabs);
}

void au_bw_paint(AuBrowserWindow *w, AuCanvas *c, AuIRect clip)
{
    clip = au_irect_intersect(clip, (AuIRect){0, 0, w->width, w->height});
    if (au_irect_empty(clip))
        return;
    AuTab *t = au_bw_active_tab(w);
    if (!w->native_chrome)
        paint_chrome(w, c, clip);
    AuIRect pg = au_irect_intersect(w->layout.page, clip);
    if (t && !au_irect_empty(pg)) {
        AuIRect p = w->layout.page;
        au_canvas_clip_push(c, (AuRect){(float)pg.x0, (float)pg.y0, (float)(pg.x1 - pg.x0), (float)(pg.y1 - pg.y0)});
        w->painting = true;
        au_page_paint(t->page, c, (AuRect){(float)p.x0, (float)p.y0, (float)(p.x1 - p.x0), (float)(p.y1 - p.y0)},
                      t->scroll_x, t->scroll_y);
        w->painting = false;
        au_canvas_clip_pop(c);
    } else if (!au_irect_empty(pg)) {
        au_fill_rect(c, (AuRect){(float)pg.x0, (float)pg.y0, (float)(pg.x1 - pg.x0), (float)(pg.y1 - pg.y0)},
                     AU_RGB(255, 255, 255));
    }
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
        w->browser->scroll_gen++;
        page_scrolled(w, t, ox, oy, true);
        changed(w, AU_CHANGED_SCROLL);
        /* Hover follows when the scrolling pauses (au_bw_idle). */
        w->rehover_pending = w->mouse_in_page;
        w->scrolled_ms = time_ms(w->browser);
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

AuTab *au_bw_open_new_tab(AuBrowserWindow *w)
{
    AuTab *t = au_bw_new_tab(w, NULL, true, w->ntabs);
    changed(w, AU_CHANGED_FOCUS_FIELD);
    return t;
}

void au_bw_reload_or_stop(AuBrowserWindow *w, bool bypass)
{
    if (au_bw_loading(w))
        au_bw_stop(w);
    else if (bypass)
        au_bw_reload_bypass(w);
    else
        au_bw_reload(w);
}

/* What a click on each part of the painted chrome does. The tab strip's
 * presses (select, drag, scroll) and the splitter act on the press
 * itself, in au_bw_mouse; AU_HIT_TAB selects here too, for a port. */
void au_bw_press(AuBrowserWindow *w, AuHit h, uint32_t mods)
{
    bool cmd = (mods & AU_MOD_COMMAND) != 0;
    switch (h.part) {
    case AU_HIT_BACK:
    case AU_HIT_FORWARD: {
        int d = h.part == AU_HIT_BACK ? -1 : 1;
        if (cmd)
            au_bw_go_new_tab(w, d);
        else
            au_bw_go(w, d);
        break;
    }
    case AU_HIT_RELOAD: au_bw_reload_or_stop(w, (mods & (AU_MOD_SHIFT | AU_MOD_OPTION)) != 0); break;
    case AU_HIT_SECURITY: au_bw_show_certificate(w); break;
    case AU_HIT_HOME: au_bw_home(w); break;
    case AU_HIT_BOOKMARK: au_bw_bookmark_page(w, true); break;
    case AU_HIT_NEW_TAB: au_bw_open_new_tab(w); break;
    case AU_HIT_TAB: au_bw_select_tab(w, h.index); break;
    case AU_HIT_TAB_CLOSE: au_bw_close_tab(w, h.index); break;
    default: break;
    }
}

static void click(AuBrowserWindow *w, AuHit h, const AuEvent *ev)
{
    if (h.part == AU_HIT_TAB)
        return; /* selected when pressed */
    au_bw_press(w, h, ev ? ev->mods : 0);
}

void au_bw_mouse_leave(AuBrowserWindow *w)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_MOUSE_MOVE;
    e.x = e.y = -100000;
    w->mouse_x = e.x;
    w->mouse_y = e.y;
    w->mouse_in_page = false;
    w->rehover_pending = false;
    set_hover(w, (AuHit){AU_HIT_NONE, -1});
    if (w->pressed.part == AU_HIT_NONE)
        page_event(w, &e); /* ends the page's hover and a link's status text */
}

AuHit au_bw_mouse(AuBrowserWindow *w, const AuEvent *ev)
{
    AuHit h = au_chrome_hit(&w->layout, ev->x, ev->y);
    if (ev->type == AU_EVENT_MOUSE_MOVE) {
        w->mouse_x = ev->x;
        w->mouse_y = ev->y;
        w->mouse_in_page = h.part == AU_HIT_PAGE;
        w->rehover_pending = false; /* this move brings hover up to date */
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
        /* An inner scrolling box under the pointer takes the wheel first.
         * Shift turns a vertical wheel horizontal. */
        AuTab *t = au_bw_active_tab(w);
        int dx = ev->dx, dy = ev->dy;
        if ((ev->mods & AU_MOD_SHIFT) && dx == 0) {
            dx = dy;
            dy = 0;
        }
        if (t && h.part == AU_HIT_PAGE &&
            au_page_scroll(t->page, ev->x - w->layout.page.x0 + t->scroll_x, ev->y - w->layout.page.y0 + t->scroll_y,
                           dx, dy))
            return h;
        au_bw_scroll_by(w, dx, dy);
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

/* The script memory a notice suggests after a page reached limit_kb: twice
 * the limit, at least 32 MB more, within the custom range; 0 when the
 * limit is already the largest a user can set. */
uint32_t au_script_oom_suggest_kb(uint32_t limit_kb)
{
    if (limit_kb >= AU_SCRIPT_MEM_CUSTOM_MAX_KB)
        return 0;
    uint32_t mb = limit_kb / 1024;
    uint32_t want = mb * 2 > mb + 32 ? mb * 2 : mb + 32;
    want = (want + 15) / 16 * 16; /* a round number of MB */
    if (want * 1024 > AU_SCRIPT_MEM_CUSTOM_MAX_KB)
        want = AU_SCRIPT_MEM_CUSTOM_MAX_KB / 1024;
    return want * 1024;
}

void au_script_oom_text(const AuScriptOom *o, const char *page_name, AuBuf *message, AuBuf *detail)
{
    au_buf_printf(message, "\"%s\" ran out of script memory.", page_name && *page_name ? page_name : "This page");
    const char *who = o->worker ? "A background script (a worker) of this page" : "This page's scripts";
    const char *them = o->worker ? "it" : "them";
    if (o->machine) {
        au_buf_printf(detail,
                      "%s needed more memory than Aura has left, so Aura stopped %s, and parts of the page may "
                      "not work. To fix it, close other tabs, windows or applications, or give Aura more memory, "
                      "then reload the page.",
                      who, them);
        return;
    }
    au_buf_printf(detail,
                  "%s needed more than the %lu MB of script memory a %s may use, so Aura stopped %s, and parts of "
                  "the page may not work. The limit keeps one page from taking all of Aura's memory.",
                  who, (unsigned long)(o->limit_kb / 1024), o->worker ? "worker" : "page", them);
    uint32_t want = au_script_oom_suggest_kb(o->limit_kb);
    if (want)
        au_buf_printf(detail,
                      " To fix it, choose Preferences > Content, set \"Script memory per %s\" to Custom with %lu MB, "
                      "and reload the page.",
                      o->worker ? "worker" : "page", (unsigned long)(want / 1024));
    else
        au_buf_puts(detail, " That is already the largest setting, so this page needs more memory than Aura can give it.");
}

static void script_oom_notice(AuBrowserWindow *w, AuPage *p, const AuScriptOom *o)
{
    AuBrowser *b = w->browser;
    if (!b->host.notice)
        return;
    AuBuf msg, det;
    au_buf_init(&msg);
    au_buf_init(&det);
    const char *name = au_page_title(p);
    au_script_oom_text(o, name[0] ? name : au_page_url(p), &msg, &det);
    b->host.notice(b->host.ctx, w, au_buf_cstr(&msg), au_buf_cstr(&det));
    au_buf_free(&msg);
    au_buf_free(&det);
}

void au_bw_idle(AuBrowserWindow *w)
{
    if (w->hscroll_dirty) {
        w->hscroll_dirty = false;
        check_hscroll(w);
    }
    if (w->rehover_pending && time_ms(w->browser) - w->scrolled_ms >= AU_REHOVER_MS) {
        w->rehover_pending = false;
        rehover(w);
    }
    au_browser_trim_documents(w->browser);
    if (w->bench)
        au_bench_idle(w->bench); /* the results stay readable until the next run */
    {
        /* The memory rules for background tabs, about once a second. */
        AuBrowser *b = w->browser;
        uint32_t t = time_ms(b);
        if (b->host.memory_free_kb && b->prefs.unload_tabs && !b->idle_depth && t - b->pressure_ms >= 1000) {
            b->pressure_ms = t;
            au_browser_memory_pressure(b, b->host.memory_free_kb(b->host.ctx));
        }
    }
    {
        /* Icon fetches run outside the pages; repaint the tabs when one
         * arrived. */
        uint32_t serial = au_favicons_idle(w->browser->favicons);
        if (serial != w->icon_serial) {
            w->icon_serial = serial;
            invalidate(w, w->layout.tabs_area);
            changed(w, AU_CHANGED_TAB_INFO);
        }
    }
    bool any = false;
    for (int i = 0; i < w->ntabs; i++) {
        /* The page being called is named, so that a low-memory unload
         * from inside the call leaves its document alone. */
        AuBrowser *b = w->browser;
        AuPage *outer = b->idle_page;
        b->idle_page = w->tabs[i]->page;
        b->idle_depth++;
        au_page_idle(w->tabs[i]->page);
        b->idle_depth--;
        b->idle_page = outer;
        if (i < w->ntabs && w->tabs[i]->loading)
            any = true;
    }
    /* Tabs opened only for a download close once it is handed over. */
    for (int i = w->ntabs - 1; i >= 0; i--)
        if (w->tabs[i]->close_pending && w->ntabs > 1)
            au_bw_close_tab(w, i);
    /* A page whose script ran out of memory says so once, here, where no
     * script is on the stack. */
    for (int i = 0; i < w->ntabs; i++) {
        AuScriptOom oom;
        if (au_page_take_script_oom(w->tabs[i]->page, &oom)) {
            script_oom_notice(w, w->tabs[i]->page, &oom);
            break; /* the tabs may have changed during the alert; the next idle looks again */
        }
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
        au_buf_printf(out, "window %d %d %d %d %d\n", w->width, w->height, w->active, w->x, w->y);
        for (int j = 0; j < w->ntabs; j++) {
            const AuTab *t = w->tabs[j];
            const AuNavList *nav = &t->nav;
            au_buf_printf(out, "tab %d\n", nav->index);
            for (int k = 0; k < nav->n; k++) {
                /* The current entry's position is the tab's own. */
                int sy = k == nav->index ? t->scroll_y : nav->e[k].scroll_y;
                au_buf_puts(out, "entry\t");
                put_clean(out, nav->e[k].url);
                au_buf_putc(out, '\t');
                put_clean(out, nav->e[k].title);
                au_buf_printf(out, "\t%d\n", sy > 0 ? sy : 0);
            }
            /* The tab's sessionStorage, unless cookies are cleared at
             * quit: then no site data is to outlive the run. */
            if (b->storage && !b->prefs.clear_cookies_on_quit)
                au_web_storage_ns_serialize(b->storage, t->storage_ns, out);
        }
    }
}

static void session_tab_free(AuSessionTab *t)
{
    au_nav_free(&t->nav);
    for (int i = 0; i < t->nstorage; i++)
        au_free(t->storage[i]);
    au_free(t->storage);
    t->storage = NULL;
    t->nstorage = 0;
}

void au_session_free(AuSession *s)
{
    for (int i = 0; i < s->nwindows; i++) {
        for (int j = 0; j < s->windows[i].ntabs; j++)
            session_tab_free(&s->windows[i].tabs[j]);
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
            int w = 0, h = 0, a = 0, x = -1, y = -1;
            /* The position came later: older files have three numbers. */
            int got = sscanf(l + 7, "%d %d %d %d %d", &w, &h, &a, &x, &y);
            if (got != 3 && got != 5) {
                ok = false;
                continue;
            }
            s->windows = au_realloc(s->windows, (size_t)(s->nwindows + 1) * sizeof *s->windows);
            AuSessionWindow *sw = &s->windows[s->nwindows++];
            memset(sw, 0, sizeof *sw);
            sw->width = w;
            sw->height = h;
            sw->active = a;
            sw->x = got == 5 ? x : -1;
            sw->y = got == 5 ? y : -1;
        } else if (!strncmp(l, "tab ", 4) && s->nwindows) {
            AuSessionWindow *sw = &s->windows[s->nwindows - 1];
            int idx = 0;
            if (sscanf(l + 4, "%d", &idx) != 1) {
                ok = false;
                continue;
            }
            sw->tabs = au_realloc(sw->tabs, (size_t)(sw->ntabs + 1) * sizeof *sw->tabs);
            AuSessionTab *st = &sw->tabs[sw->ntabs++];
            memset(st, 0, sizeof *st);
            au_nav_init(&st->nav);
            st->nav.index = idx; /* checked after its entries are read */
        } else if (!strncmp(l, "storage\t", 8) && s->nwindows && s->windows[s->nwindows - 1].ntabs) {
            /* The tab's sessionStorage for one origin; the storage
             * service reads the line when the tab is restored. */
            AuSessionWindow *sw = &s->windows[s->nwindows - 1];
            AuSessionTab *st = &sw->tabs[sw->ntabs - 1];
            if (st->nstorage >= AU_SESSION_STORAGE_LINES) {
                ok = false;
                continue;
            }
            st->storage = au_realloc(st->storage, (size_t)(st->nstorage + 1) * sizeof *st->storage);
            st->storage[st->nstorage++] = au_strndup(l + 8, line.len - 8);
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
            /* After the title, optionally, the scroll position. */
            char *title = tab ? tab + 1 : (char *)"";
            char *tab2 = tab ? strchr(title, '\t') : NULL;
            int sy = 0;
            if (tab2) {
                *tab2 = 0;
                sy = atoi(tab2 + 1);
                if (sy < 0)
                    sy = 0;
            }
            int want = nav->index;
            nav->index = nav->n - 1;
            au_nav_push(nav, url, title);
            nav->e[nav->index].scroll_y = sy;
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
                session_tab_free(&sw->tabs[j]);
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
        for (int k = 0; k < src->n; k++) {
            au_nav_push(&t->nav, src->e[k].url, src->e[k].title);
            t->nav.e[t->nav.n - 1].scroll_y = src->e[k].scroll_y;
        }
        t->nav.index = src->index;
        /* Its sessionStorage, before any document of the tab runs. */
        for (int k = 0; k < sw->tabs[j].nstorage && w->browser->storage; k++)
            au_web_storage_ns_restore(w->browser->storage, t->storage_ns, sw->tabs[j].storage[k],
                                      strlen(sw->tabs[j].storage[k]));
        AuNavEntry *e = au_nav_current(&t->nav);
        if (e && j != sw->active && w->browser->prefs.unload_restored && !au_url_scheme_is(e->url, "about")) {
            /* A background tab comes back unloaded: it loads when it is
             * first selected, so a session of many tabs starts as fast
             * as one of a single tab. */
            apply_site_zoom(t, e->url);
            au_page_set_discarded(t->page, e->url, e->title);
            t->scroll_y = e->scroll_y;
        } else if (e) {
            /* Back where it was left, once the page is tall enough. */
            char *url = au_strndup(e->url, strlen(e->url));
            apply_site_zoom(t, url);
            entry_load(t);
            au_page_navigate_entry(t->page, url, 0, e->scroll_y);
            au_free(url);
        }
    }
    relayout(w, true);
    if (sw->ntabs)
        au_bw_select_tab(w, first + sw->active);
}
