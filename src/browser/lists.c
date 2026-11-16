/* The list windows' models. See lists.h. */
#include "lists.h"

#include "browser.h"
#include "favicon.h"

#include <stdio.h>
#include <string.h>

/* ---- What each window has ---- */

static const char *const bookmark_cols[] = {"Name", "Address", "Added"};
static const char *const history_cols[] = {"Title", "Time", "Address", "Visits"};
static const char *const download_cols[] = {"Name", "Address", "Status", "Progress", "Size"};
static const char *const cookie_cols[] = {"Name", "Domain", "Path", "Expires", "Details"};

static const AuListButton bookmark_buttons[] = {
    {AU_LIST_OPEN, "Open", 64},
    {AU_LIST_NEW_FOLDER, "New Folder", 96},
    {AU_LIST_RENAME, "Rename...", 86},
    {AU_LIST_DELETE, "Delete", 70},
};
static const AuListButton history_buttons[] = {
    {AU_LIST_OPEN, "Open", 64},
    {AU_LIST_DELETE, "Delete", 70},
    {AU_LIST_CLEAR, "Clear History", 110},
};
static const AuListButton download_buttons[] = {
    {AU_LIST_STOP, "Stop", 58},
    {AU_LIST_RESUME, "Resume", 72},
    {AU_LIST_DELETE, "Remove", 72},
    {AU_LIST_CLEAR, "Clear Completed", 124},
    {AU_LIST_REVEAL, "Reveal", 66},
};
static const AuListButton cookie_buttons[] = {
    {AU_LIST_DETAILS, "Details...", 86},
    {AU_LIST_DELETE, "Remove", 72},
    {AU_LIST_CLEAR, "Remove All", 96},
    {AU_LIST_SITE_RULE, "Site Rule...", 100},
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

const char *au_list_title(AuListKind kind)
{
    switch (kind) {
    case AU_LIST_BOOKMARKS: return "Bookmarks";
    case AU_LIST_HISTORY: return "History";
    case AU_LIST_DOWNLOADS: return "Downloads";
    case AU_LIST_COOKIES: return "Cookies";
    default: return "";
    }
}

int au_list_columns(AuListKind kind, const char *const **headings)
{
    const char *const *h = NULL;
    int n = 0;
    switch (kind) {
    case AU_LIST_BOOKMARKS: h = bookmark_cols, n = COUNT(bookmark_cols); break;
    case AU_LIST_HISTORY: h = history_cols, n = COUNT(history_cols); break;
    case AU_LIST_DOWNLOADS: h = download_cols, n = COUNT(download_cols); break;
    case AU_LIST_COOKIES: h = cookie_cols, n = COUNT(cookie_cols); break;
    default: break;
    }
    if (headings)
        *headings = h;
    return n;
}

int au_list_buttons(AuListKind kind, const AuListButton **buttons)
{
    const AuListButton *b = NULL;
    int n = 0;
    switch (kind) {
    case AU_LIST_BOOKMARKS: b = bookmark_buttons, n = COUNT(bookmark_buttons); break;
    case AU_LIST_HISTORY: b = history_buttons, n = COUNT(history_buttons); break;
    case AU_LIST_DOWNLOADS: b = download_buttons, n = COUNT(download_buttons); break;
    case AU_LIST_COOKIES: b = cookie_buttons, n = COUNT(cookie_buttons); break;
    default: break;
    }
    if (buttons)
        *buttons = b;
    return n;
}

/* ---- Sources ---- */

void au_list_sources(AuListSources *s, struct AuBrowser *b, AuCookieJar *jar, AuWebStorage *storage)
{
    memset(s, 0, sizeof *s);
    if (b) {
        s->bookmarks = &b->bookmarks;
        s->history = &b->history;
        s->downloads = &b->downloads;
        s->icons = b->favicons;
        s->browser = b;
    }
    s->jar = jar;
    s->storage = storage;
}

static int32_t utc_offset(const AuListModel *m)
{
    return m->src.history ? m->src.history->utc_offset : 0;
}

/* The sources' change signature. For cookies: cookies plus rules, and
 * the storage areas with their size in KB (so a page that writes all
 * the time does not rebuild the list at every pass). */
static uint32_t signature(const AuListModel *m)
{
    switch (m->kind) {
    case AU_LIST_BOOKMARKS: return m->src.bookmarks ? m->src.bookmarks->generation : 0;
    case AU_LIST_HISTORY: return m->src.history ? m->src.history->generation : 0;
    case AU_LIST_DOWNLOADS: return m->src.downloads ? m->src.downloads->generation : 0;
    case AU_LIST_COOKIES: {
        AuCookieJar *j = m->src.jar;
        uint32_t sig = j ? (uint32_t)(au_cookie_jar_count(j) * 1000u + au_cookie_rules_count(j)) : 0;
        AuWebStorage *ws = m->src.storage;
        if (ws) {
            AuWebStorageStats st;
            au_web_storage_stats(ws, &st);
            sig += (uint32_t)au_web_storage_count(ws) * 7919u + (uint32_t)(st.bytes >> 10) * 104729u;
        }
        return sig;
    }
    default: return 0;
    }
}

/* ---- Rows ---- */

static char *dup(const char *s)
{
    return au_strndup(s ? s : "", s ? strlen(s) : 0);
}

static void free_item(AuListItem *it)
{
    for (int c = 0; c < AU_LIST_MAX_COLS; c++)
        au_free(it->cols[c]);
    au_free(it->summary);
    au_free(it->icon_host);
}

static void clear_items(AuListModel *m)
{
    for (int i = 0; i < m->n; i++)
        free_item(&m->items[i]);
    m->n = 0;
}

static AuListItem *add_item(AuListModel *m, AuListItemType type, int depth, bool expanded, AuListIcon icon,
                            uint32_t id)
{
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 32;
        m->items = au_realloc(m->items, (size_t)m->cap * sizeof *m->items);
    }
    AuListItem *it = &m->items[m->n++];
    memset(it, 0, sizeof *it);
    it->id = id;
    it->type = type;
    it->depth = depth;
    it->expanded = expanded;
    it->icon = icon;
    it->progress = -1;
    it->state = -1;
    int ncols = au_list_columns(m->kind, NULL);
    for (int c = 0; c < AU_LIST_MAX_COLS; c++)
        it->cols[c] = c < ncols ? dup("") : NULL;
    it->summary = dup("");
    return it;
}

static void set_col(AuListItem *it, int c, const char *text)
{
    au_free(it->cols[c]);
    it->cols[c] = dup(text);
}

static void set_summary(AuListItem *it, const char *text)
{
    au_free(it->summary);
    it->summary = dup(text);
}

/* Notes url's host on a page row when the icon store has its icon. */
static void row_icon(const AuListModel *m, AuListItem *it, const char *url)
{
    char host[256];
    if (!m->src.icons || !url || !au_favicons_host(url, host, sizeof host) ||
        !au_favicons_for_host(m->src.icons, host))
        return;
    it->icon_host = dup(host);
}

static void build_bookmarks(AuListModel *m)
{
    AuBookmarks *bm = m->src.bookmarks;
    if (!bm)
        return;
    const AuBookmark *root = &bm->root;
    char when[32];
    for (const AuBookmark *b = root->first; b;) {
        AuListItem *it = add_item(m, b->folder ? AU_LIST_ITEM_FOLDER : AU_LIST_ITEM_ENTRY, au_bookmark_depth(b),
                                  b->folder && b->expanded, b->folder ? AU_LIST_ICON_FOLDER : AU_LIST_ICON_PAGE,
                                  b->id);
        set_col(it, 0, b->title);
        if (!b->folder) {
            set_col(it, 1, b->url);
            set_summary(it, b->url);
            row_icon(m, it, b->url);
        }
        if (b->added > 0) {
            au_cookie_format_time(b->added + utc_offset(m), when, sizeof when);
            set_col(it, 2, when);
        }
        b = au_bookmark_next(b, root, b->folder && !b->expanded);
    }
}

static void build_history(AuListModel *m, AuTime now)
{
    const AuHistory *h = m->src.history;
    if (!h)
        return;
    int64_t today = au_day_of(now + h->utc_offset);
    int64_t day = 0;
    char label[96], when[16], visits[16];
    AuBuf summary;
    au_buf_init(&summary);
    for (size_t i = 0; i < h->n; i++) {
        const AuHistoryEntry *e = &h->e[i];
        AuTime local = e->last_visit + h->utc_offset;
        int64_t d = au_day_of(local);
        if (i == 0 || d != day) {
            day = d;
            au_day_label(d, today, label, sizeof label);
            AuListItem *g = add_item(m, AU_LIST_ITEM_GROUP, 0, true, AU_LIST_ICON_NONE,
                                     AU_LIST_ID_GROUP | (uint32_t)(d & 0x7FFFFFFF));
            set_col(g, 0, label);
        }
        int64_t secs = local - d * 86400;
        snprintf(when, sizeof when, "%02d:%02d", (int)(secs / 3600), (int)(secs / 60 % 60));
        snprintf(visits, sizeof visits, "%u", (unsigned)e->visits);
        AuListItem *it = add_item(m, AU_LIST_ITEM_ENTRY, 1, false, AU_LIST_ICON_PAGE, e->id);
        set_col(it, 0, *e->title ? e->title : e->url);
        set_col(it, 1, when);
        set_col(it, 2, e->url);
        set_col(it, 3, visits);
        au_buf_clear(&summary);
        au_buf_puts(&summary, when);
        au_buf_puts(&summary, "  ");
        au_buf_puts(&summary, e->url);
        set_summary(it, au_buf_cstr(&summary));
        row_icon(m, it, e->url);
    }
    au_buf_free(&summary);
}

static void build_downloads(AuListModel *m)
{
    const AuDownloads *dl = m->src.downloads;
    if (!dl)
        return;
    char status[300], size[32], pct[16];
    for (int i = 0; i < dl->n; i++) {
        const AuDownload *d = dl->items[i];
        AuListItem *it = add_item(m, AU_LIST_ITEM_ENTRY, 0, false, AU_LIST_ICON_FILE, d->id);
        it->state = (int)d->state;
        set_col(it, 0, d->filename);
        set_col(it, 1, d->url);
        au_download_status(d, status, sizeof status);
        set_col(it, 2, status);
        set_summary(it, status);
        if (d->total > 0) {
            int64_t p = d->received >= d->total ? 1000 : d->received * 1000 / d->total;
            it->progress = p < 0 ? 0 : (int)p;
            snprintf(pct, sizeof pct, "%d%%", it->progress / 10);
            set_col(it, 3, pct);
        }
        size[0] = 0;
        if (d->total >= 0)
            au_format_size(d->total, size, sizeof size);
        else if (d->received > 0)
            au_format_size(d->received, size, sizeof size);
        set_col(it, 4, size);
    }
}

/* FNV-1a over a string and a separator. */
static uint32_t hash_str(uint32_t h, const char *s)
{
    for (; s && *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return (h ^ 0xFFu) * 16777619u;
}

enum { ID_SITE = 0x40000000u, ID_COOKIE = 0x80000000u, ID_STORAGE = 0xC0000000u };

static void build_cookies(AuListModel *m, AuTime now)
{
    if (!m->have_cv)
        return;
    AuCookieView *v = &m->cv;
    v->utc_offset = utc_offset(m);
    v->list.selected = m->selected;
    au_cookie_view_refresh(v, now);
    char buf[300];
    for (int r = 0; r < v->list.n; r++) {
        const AuListRow *row = &v->list.rows[r];
        AuListItem *it;
        if (row->id & AU_COOKIE_ROW_SITE) {
            it = add_item(m, AU_LIST_ITEM_FOLDER, 0, row->expanded, AU_LIST_ICON_FOLDER,
                          ID_SITE | (hash_str(2166136261u, row->text) & 0x3FFFFFFF));
        } else if (row->id & AU_COOKIE_ROW_STORAGE) {
            const AuCookieViewArea *a = &v->areas[row->id & ~AU_COOKIE_ROW_STORAGE];
            uint32_t h = hash_str(2166136261u, a->key);
            h = (h ^ (uint32_t)a->kind) * 16777619u;
            h = (h ^ a->ns) * 16777619u;
            it = add_item(m, AU_LIST_ITEM_ENTRY, row->indent, false, AU_LIST_ICON_STORAGE,
                          ID_STORAGE | (h & 0x3FFFFFFF));
            set_col(it, 1, a->key);
        } else {
            const AuCookie *c = au_cookie_view_cookie(v, r);
            uint32_t h = 2166136261u;
            if (c) {
                h = hash_str(h, c->domain);
                h = hash_str(h, c->path);
                h = hash_str(h, c->name);
                h = hash_str(h, c->partition);
            }
            it = add_item(m, AU_LIST_ITEM_ENTRY, row->indent, false, AU_LIST_ICON_COOKIE,
                          ID_COOKIE | (h & 0x3FFFFFFF));
            if (c) {
                snprintf(buf, sizeof buf, "%s%s", c->host_only ? "" : ".", c->domain);
                set_col(it, 1, buf);
                set_col(it, 2, c->path);
                if (c->expires)
                    au_cookie_format_time(c->expires + v->utc_offset, buf, sizeof buf);
                else
                    snprintf(buf, sizeof buf, "session");
                set_col(it, 3, buf);
            }
        }
        set_col(it, 0, row->text);
        set_col(it, 4, row->detail);
        set_summary(it, row->detail);
    }
    m->selected = v->list.selected;
}

/* ---- The model ---- */

void au_list_init(AuListModel *m, AuListKind kind, const AuListSources *src, const AuListHost *host, AuTime now)
{
    memset(m, 0, sizeof *m);
    m->kind = kind;
    if (src)
        m->src = *src;
    if (host)
        m->host = *host;
    m->selected = -1;
    if (kind == AU_LIST_COOKIES && m->src.jar) {
        au_cookie_view_init(&m->cv, m->src.jar);
        m->cv.storage = m->src.storage;
        m->have_cv = true;
    }
    au_list_refresh(m, now);
}

void au_list_free(AuListModel *m)
{
    clear_items(m);
    au_free(m->items);
    if (m->have_cv)
        au_cookie_view_free(&m->cv);
    memset(m, 0, sizeof *m);
    m->selected = -1;
}

bool au_list_stale(const AuListModel *m)
{
    return signature(m) != m->seen || m->touched != m->seen_touched;
}

void au_list_touch(AuListModel *m)
{
    m->touched++;
}

int au_list_find(const AuListModel *m, uint32_t id)
{
    if (!id)
        return -1;
    for (int i = 0; i < m->n; i++)
        if (m->items[i].id == id)
            return i;
    return -1;
}

void au_list_refresh(AuListModel *m, AuTime now)
{
    int old = m->selected >= 0 && m->selected < m->n ? m->selected : -1;
    uint32_t keep = old >= 0 ? m->items[old].id : 0;
    clear_items(m);
    switch (m->kind) {
    case AU_LIST_BOOKMARKS: build_bookmarks(m); break;
    case AU_LIST_HISTORY: build_history(m, now); break;
    case AU_LIST_DOWNLOADS: build_downloads(m); break;
    case AU_LIST_COOKIES: build_cookies(m, now); break; /* the cookie view keeps its own selection */
    default: break;
    }
    if (m->kind != AU_LIST_COOKIES) {
        int s = au_list_find(m, keep);
        if (s < 0 && old >= 0 && (m->kind == AU_LIST_HISTORY || m->kind == AU_LIST_DOWNLOADS))
            s = old < m->n ? old : m->n - 1;
        m->selected = s;
    }
    if (m->selected >= m->n)
        m->selected = m->n - 1;
    m->seen = signature(m);
    m->seen_touched = m->touched;
    m->generation++;
}

void au_list_select(AuListModel *m, int row)
{
    m->selected = row >= 0 && row < m->n ? row : -1;
    if (m->have_cv)
        m->cv.list.selected = m->selected;
}

static const AuListItem *sel_item(const AuListModel *m)
{
    return m->selected >= 0 && m->selected < m->n ? &m->items[m->selected] : NULL;
}

static AuBookmark *bookmark_of(const AuListModel *m, const AuListItem *it)
{
    return it && m->src.bookmarks ? au_bookmark_find_id(m->src.bookmarks, it->id) : NULL;
}

static long history_index(const AuListModel *m, const AuListItem *it)
{
    const AuHistory *h = m->src.history;
    if (!it || !h || it->type != AU_LIST_ITEM_ENTRY)
        return -1;
    for (size_t i = 0; i < h->n; i++)
        if (h->e[i].id == it->id)
            return (long)i;
    return -1;
}

static AuDownload *download_of(const AuListModel *m, const AuListItem *it)
{
    return it && m->src.downloads ? au_downloads_find(m->src.downloads, it->id) : NULL;
}

bool au_list_can(const AuListModel *m, AuListAction a)
{
    const AuListItem *it = sel_item(m);
    switch (m->kind) {
    case AU_LIST_BOOKMARKS:
        switch (a) {
        case AU_LIST_OPEN:
        case AU_LIST_RENAME:
        case AU_LIST_DELETE: return bookmark_of(m, it) != NULL;
        case AU_LIST_OPEN_NEW_TAB: return it && it->type == AU_LIST_ITEM_ENTRY && bookmark_of(m, it);
        case AU_LIST_NEW_FOLDER: return m->src.bookmarks != NULL;
        default: return false;
        }
    case AU_LIST_HISTORY:
        switch (a) {
        case AU_LIST_OPEN:
        case AU_LIST_OPEN_NEW_TAB:
        case AU_LIST_DELETE: return history_index(m, it) >= 0;
        case AU_LIST_CLEAR: return m->src.history && m->src.history->n > 0;
        default: return false;
        }
    case AU_LIST_DOWNLOADS: {
        const AuDownload *d = download_of(m, it);
        switch (a) {
        case AU_LIST_STOP: return d && (d->state == AU_DL_ACTIVE || d->state == AU_DL_QUEUED);
        case AU_LIST_RESUME: return d && (d->state == AU_DL_PAUSED || d->state == AU_DL_FAILED);
        case AU_LIST_DELETE: return d != NULL;
        case AU_LIST_REVEAL: return d && d->state == AU_DL_DONE;
        case AU_LIST_CLEAR: {
            const AuDownloads *dl = m->src.downloads;
            for (int k = 0; dl && k < dl->n; k++)
                if (dl->items[k]->state >= AU_DL_DONE)
                    return true;
            return false;
        }
        default: return false;
        }
    }
    case AU_LIST_COOKIES:
        if (!m->have_cv)
            return false;
        switch (a) {
        case AU_LIST_DETAILS:
        case AU_LIST_DELETE:
        case AU_LIST_SITE_RULE: return it != NULL;
        case AU_LIST_CLEAR:
            return au_cookie_jar_count(m->src.jar) > 0 || (m->src.storage && au_web_storage_count(m->src.storage) > 0);
        default: return false;
        }
    default: return false;
    }
}

static void bookmarks_changed(AuListModel *m)
{
    if (m->src.browser)
        au_browser_bookmarks_changed(m->src.browser);
}

static void history_changed(AuListModel *m)
{
    if (m->src.browser)
        au_browser_history_changed(m->src.browser);
}

static void open_url(AuListModel *m, const char *url, bool new_tab)
{
    if (!m->host.open_url || !url)
        return;
    /* Opening a page can change the list the URL came from. */
    char *u = dup(url);
    m->host.open_url(m->host.ctx, u, new_tab);
    au_free(u);
}

static char *ask(AuListModel *m, const char *prompt, const char *initial)
{
    return m->host.ask_text ? m->host.ask_text(m->host.ctx, prompt, initial) : NULL;
}

static AuListResult details(AuListModel *m, AuTime now)
{
    const AuListItem *it = sel_item(m);
    if (!it || !m->have_cv || !m->host.show_text)
        return AU_LIST_NOTHING;
    AuBuf d;
    au_buf_init(&d);
    m->cv.list.selected = m->selected;
    au_cookie_view_describe(&m->cv, m->selected, &d, now);
    char *title = dup(it->cols[0]);
    m->host.show_text(m->host.ctx, title, au_buf_cstr(&d));
    au_free(title);
    au_buf_free(&d);
    return AU_LIST_DONE;
}

void au_list_toggle(AuListModel *m, int row, AuTime now)
{
    if (row < 0 || row >= m->n || m->items[row].type != AU_LIST_ITEM_FOLDER)
        return;
    if (m->kind == AU_LIST_BOOKMARKS) {
        AuBookmark *b = bookmark_of(m, &m->items[row]);
        if (b && b->folder)
            b->expanded = !b->expanded;
    } else if (m->kind == AU_LIST_COOKIES && m->have_cv) {
        /* The site row becomes the selection, as a click on it would. */
        m->selected = row;
        m->cv.list.selected = row;
        au_cookie_view_toggle(&m->cv, row, now);
        m->selected = m->cv.list.selected;
    }
    au_list_refresh(m, now);
}

AuListResult au_list_activate(AuListModel *m, int row, AuTime now)
{
    if (row < 0 || row >= m->n)
        return AU_LIST_NOTHING;
    au_list_select(m, row);
    const AuListItem *it = &m->items[row];
    switch (m->kind) {
    case AU_LIST_BOOKMARKS: {
        AuBookmark *b = bookmark_of(m, it);
        if (!b)
            return AU_LIST_NOTHING;
        if (b->folder)
            au_list_toggle(m, row, now);
        else
            open_url(m, b->url, false);
        return AU_LIST_DONE;
    }
    case AU_LIST_HISTORY: {
        long i = history_index(m, it);
        if (i < 0)
            return AU_LIST_NOTHING;
        open_url(m, m->src.history->e[i].url, false);
        return AU_LIST_DONE;
    }
    case AU_LIST_COOKIES:
        if (it->type == AU_LIST_ITEM_FOLDER) {
            au_list_toggle(m, row, now);
            return AU_LIST_DONE;
        }
        return details(m, now); /* a cookie or a storage area */
    default: return AU_LIST_NOTHING;
    }
}

static const char *const rule_names[] = {"Use the cookie preferences", "Always allow", "Session only", "Block"};

bool au_list_set_site_rule(AuListModel *m, AuCookieRule rule, AuTime now)
{
    if (m->kind != AU_LIST_COOKIES || !m->have_cv || !sel_item(m) || rule < AU_COOKIE_RULE_NONE ||
        rule > AU_COOKIE_RULE_BLOCK)
        return false;
    m->cv.list.selected = m->selected;
    if (!au_cookie_view_site(&m->cv, m->selected))
        return false;
    au_cookie_view_set_rule(&m->cv, m->selected, rule, now);
    m->selected = m->cv.list.selected;
    if (m->host.cookie_rules_changed)
        m->host.cookie_rules_changed(m->host.ctx);
    au_list_refresh(m, now);
    return true;
}

static AuListResult site_rule(AuListModel *m, AuTime now)
{
    m->cv.list.selected = m->selected;
    const char *site = au_cookie_view_site(&m->cv, m->selected);
    if (!site || !m->host.choose)
        return AU_LIST_NOTHING;
    int cur = (int)au_cookie_jar_rule(m->src.jar, site);
    int choice = m->host.choose(m->host.ctx, AU_LIST_SITE_RULE, rule_names, COUNT(rule_names), cur);
    if (choice < 0 || choice >= COUNT(rule_names) || choice == cur)
        return AU_LIST_NOTHING;
    return au_list_set_site_rule(m, (AuCookieRule)choice, now) ? AU_LIST_DONE : AU_LIST_NOTHING;
}

static AuListResult new_folder(AuListModel *m, AuTime now)
{
    AuBookmarks *bm = m->src.bookmarks;
    AuBookmark *b = bookmark_of(m, sel_item(m));
    AuBookmark *parent = b ? (b->folder ? b : b->parent) : NULL;
    if (parent == &bm->root)
        parent = NULL;
    char *name = ask(m, "Name of the new folder:", "New Folder");
    if (!name)
        return AU_LIST_NOTHING;
    /* The dialog let other code run: look the parent up again. */
    uint32_t parent_id = parent ? parent->id : 0;
    parent = parent_id ? au_bookmark_find_id(bm, parent_id) : NULL;
    if (parent)
        parent->expanded = true;
    AuBookmark *f = au_bookmark_add_folder(bm, parent, NULL, *name ? name : "New Folder", now);
    au_free(name);
    uint32_t id = f->id;
    bookmarks_changed(m);
    au_list_refresh(m, now);
    au_list_select(m, au_list_find(m, id));
    return AU_LIST_DONE;
}

static AuListResult rename_bookmark(AuListModel *m, AuTime now)
{
    AuBookmark *b = bookmark_of(m, sel_item(m));
    if (!b)
        return AU_LIST_NOTHING;
    uint32_t id = b->id;
    char *name = ask(m, "New name:", b->title);
    b = au_bookmark_find_id(m->src.bookmarks, id);
    AuListResult r = AU_LIST_NOTHING;
    if (b && name && *name) {
        au_bookmark_rename(m->src.bookmarks, b, name);
        bookmarks_changed(m);
        r = AU_LIST_DONE;
    }
    au_free(name);
    au_list_refresh(m, now);
    return r;
}

static AuListResult delete_row(AuListModel *m, AuTime now)
{
    const AuListItem *it = sel_item(m);
    switch (m->kind) {
    case AU_LIST_BOOKMARKS:
        au_bookmark_delete(m->src.bookmarks, bookmark_of(m, it));
        bookmarks_changed(m);
        break;
    case AU_LIST_HISTORY:
        au_history_remove(m->src.history, (size_t)history_index(m, it));
        history_changed(m);
        break;
    case AU_LIST_DOWNLOADS: au_download_remove(m->src.downloads, download_of(m, it)); break;
    case AU_LIST_COOKIES:
        m->cv.list.selected = m->selected;
        au_cookie_view_remove(&m->cv, m->selected, now);
        m->selected = m->cv.list.selected;
        break;
    default: return AU_LIST_NOTHING;
    }
    au_list_refresh(m, now);
    return AU_LIST_DONE;
}

static AuListResult clear(AuListModel *m, AuTime now)
{
    switch (m->kind) {
    case AU_LIST_HISTORY:
        au_history_clear(m->src.history);
        if (m->src.icons)
            au_favicons_clear(m->src.icons); /* they name visited hosts */
        m->selected = -1;
        history_changed(m);
        break;
    case AU_LIST_DOWNLOADS: au_downloads_clear_completed(m->src.downloads); break;
    case AU_LIST_COOKIES:
        m->cv.list.selected = m->selected;
        au_cookie_view_remove_all(&m->cv, now);
        m->selected = m->cv.list.selected;
        break;
    default: return AU_LIST_NOTHING;
    }
    au_list_refresh(m, now);
    return AU_LIST_DONE;
}

AuListResult au_list_run(AuListModel *m, AuListAction a, AuTime now)
{
    if (!au_list_can(m, a))
        return AU_LIST_NOTHING;
    const AuListItem *it = sel_item(m);
    switch (a) {
    case AU_LIST_OPEN: return au_list_activate(m, m->selected, now);
    case AU_LIST_OPEN_NEW_TAB: {
        if (m->kind == AU_LIST_BOOKMARKS)
            open_url(m, bookmark_of(m, it)->url, true);
        else
            open_url(m, m->src.history->e[history_index(m, it)].url, true);
        return AU_LIST_DONE;
    }
    case AU_LIST_NEW_FOLDER: return new_folder(m, now);
    case AU_LIST_RENAME: return rename_bookmark(m, now);
    case AU_LIST_DELETE: return delete_row(m, now);
    case AU_LIST_CLEAR: return clear(m, now);
    case AU_LIST_STOP:
        au_download_pause(m->src.downloads, download_of(m, it));
        au_list_refresh(m, now);
        return AU_LIST_DONE;
    case AU_LIST_RESUME:
        au_download_resume(m->src.downloads, download_of(m, it));
        au_list_refresh(m, now);
        return AU_LIST_DONE;
    case AU_LIST_REVEAL: {
        const AuDownload *d = download_of(m, it);
        if (!m->host.reveal_download || !m->host.reveal_download(m->host.ctx, d))
            return AU_LIST_FAILED;
        return AU_LIST_DONE;
    }
    case AU_LIST_DETAILS: return details(m, now);
    case AU_LIST_SITE_RULE: return site_rule(m, now);
    default: return AU_LIST_NOTHING;
    }
}

bool au_list_move_bookmark(AuListModel *m, uint32_t id, uint32_t folder, uint32_t before, AuTime now)
{
    AuBookmarks *bm = m->src.bookmarks;
    if (m->kind != AU_LIST_BOOKMARKS || !bm)
        return false;
    AuBookmark *b = au_bookmark_find_id(bm, id);
    AuBookmark *f = folder ? au_bookmark_find_id(bm, folder) : &bm->root;
    AuBookmark *at = before ? au_bookmark_find_id(bm, before) : NULL;
    if (!b || !f || !f->folder || (before && (!at || at->parent != f)))
        return false;
    if (!au_bookmark_move(bm, b, f, at))
        return false;
    bookmarks_changed(m);
    au_list_refresh(m, now);
    return true;
}
