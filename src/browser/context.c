/* Contextual menus (context.h). */
#include "context.h"
#include "url.h"

#include <string.h>

static void add(AuContextMenu *m, AuCommand cmd, const char *label, bool enabled)
{
    if (m->n >= AU_CONTEXT_MAX)
        return;
    /* No separator first, last-but-pending or twice in a row. */
    if (cmd == AU_CMD_SEPARATOR && (m->n == 0 || m->items[m->n - 1].cmd == AU_CMD_SEPARATOR))
        return;
    m->items[m->n].cmd = cmd;
    m->items[m->n].label = label;
    m->items[m->n].enabled = enabled;
    m->n++;
}

static void sep(AuContextMenu *m)
{
    add(m, AU_CMD_SEPARATOR, "-", false);
}

static char *take(AuBuf *b)
{
    if (!b->len) {
        au_buf_free(b);
        return NULL;
    }
    au_buf_cstr(b);
    return au_buf_take(b, NULL);
}

/* Save Link As and Save Image As download http and https addresses. */
static bool saveable(const char *url)
{
    return au_url_scheme_is(url, "http") || au_url_scheme_is(url, "https");
}

bool au_bw_context_menu(AuBrowserWindow *w, int x, int y, unsigned flags, AuContextMenu *m)
{
    memset(m, 0, sizeof *m);
    m->tab = -1;
    AuHit h = au_chrome_hit(&w->layout, x, y);
    AuTab *t = au_bw_active_tab(w);
    switch (h.part) {
    case AU_HIT_TAB:
    case AU_HIT_TAB_CLOSE:
        if (h.index < 0 || h.index >= w->ntabs)
            break;
        m->kind = AU_CONTEXT_TAB;
        m->tab = h.index;
        add(m, AU_CMD_CLOSE_TAB, "Close Tab", true);
        add(m, AU_CMD_CLOSE_OTHER_TABS, "Close Other Tabs", w->ntabs > 1);
        sep(m);
        add(m, AU_CMD_RELOAD_TAB, "Reload", true);
        add(m, AU_CMD_DUPLICATE_TAB, "Duplicate", true);
        break;
    case AU_HIT_ADDRESS:
    case AU_HIT_SECURITY: {
        bool sel = (flags & AU_CONTEXT_FIELD_SELECTION) != 0, clip = (flags & AU_CONTEXT_CLIPBOARD_TEXT) != 0;
        m->kind = AU_CONTEXT_FIELD;
        add(m, AU_CMD_CUT, "Cut", sel);
        add(m, AU_CMD_FIELD_COPY, "Copy", sel);
        add(m, AU_CMD_PASTE, "Paste", clip);
        add(m, AU_CMD_PASTE_GO, "Paste and Go", clip);
        sep(m);
        add(m, AU_CMD_SELECT_ALL, "Select All", (flags & AU_CONTEXT_FIELD_TEXT) != 0);
        break;
    }
    case AU_HIT_PAGE: {
        if (!t)
            break;
        m->kind = AU_CONTEXT_PAGE;
        AuBuf link, image;
        au_buf_init(&link);
        au_buf_init(&image);
        au_page_hit_info(t->page, x - w->layout.page.x0 + t->scroll_x, y - w->layout.page.y0 + t->scroll_y, &link,
                         &image);
        m->link = take(&link);
        m->image = take(&image);
        if (m->link) {
            add(m, AU_CMD_OPEN_LINK, "Open Link", true);
            add(m, AU_CMD_OPEN_LINK_TAB, "Open Link in New Tab", true);
            add(m, AU_CMD_OPEN_LINK_WINDOW, "Open Link in New Window", true);
            sep(m);
            add(m, AU_CMD_COPY_LINK, "Copy Link Address", true);
            if (saveable(m->link))
                add(m, AU_CMD_SAVE_LINK, "Save Link As", true);
            add(m, AU_CMD_BOOKMARK_LINK, "Bookmark Link", true);
        }
        if (m->image) {
            sep(m);
            add(m, AU_CMD_OPEN_IMAGE_TAB, "Open Image in New Tab", true);
            add(m, AU_CMD_COPY_IMAGE, "Copy Image Address", true);
            if (saveable(m->image))
                add(m, AU_CMD_SAVE_IMAGE, "Save Image As", true);
        }
        if (!m->link && !m->image) {
            add(m, AU_CMD_BACK, "Back", au_nav_can_go(&t->nav, -1));
            add(m, AU_CMD_FORWARD, "Forward", au_nav_can_go(&t->nav, 1));
            add(m, AU_CMD_RELOAD, "Reload", true);
            sep(m);
            add(m, AU_CMD_VIEW_SOURCE, "View Source", au_bw_can_view_source(w));
            add(m, AU_CMD_BOOKMARK_PAGE, "Bookmark Page", true);
        }
        if (au_page_has_selection(t->page)) {
            sep(m);
            add(m, AU_CMD_COPY, "Copy", true);
        }
        break;
    }
    default:
        break;
    }
    return m->kind != AU_CONTEXT_NONE;
}

void au_context_menu_free(AuContextMenu *m)
{
    au_free(m->link);
    au_free(m->image);
    m->link = m->image = NULL;
    m->n = 0;
    m->kind = AU_CONTEXT_NONE;
}

bool au_bw_context_command(AuBrowserWindow *w, const AuContextMenu *m, AuCommand cmd)
{
    AuTab *t = au_bw_active_tab(w);
    switch (cmd) {
    case AU_CMD_OPEN_LINK:
        if (m->link)
            au_bw_navigate(w, m->link);
        return true;
    case AU_CMD_OPEN_LINK_TAB:
        if (m->link)
            au_bw_new_tab(w, m->link, false, -1);
        return true;
    case AU_CMD_BOOKMARK_LINK:
        if (m->link)
            au_browser_bookmark_url(w->browser, m->link, NULL);
        return true;
    case AU_CMD_OPEN_IMAGE_TAB:
        if (m->image)
            au_bw_new_tab(w, m->image, false, -1);
        return true;
    case AU_CMD_SAVE_LINK:
        if (m->link)
            au_bw_save_url(w, m->link);
        return true;
    case AU_CMD_SAVE_IMAGE:
        if (m->image)
            au_bw_save_url(w, m->image);
        return true;
    case AU_CMD_BACK: au_bw_back(w); return true;
    case AU_CMD_FORWARD: au_bw_forward(w); return true;
    case AU_CMD_RELOAD: au_bw_reload(w); return true;
    case AU_CMD_VIEW_SOURCE: au_bw_view_source(w); return true;
    case AU_CMD_BOOKMARK_PAGE:
        if (t)
            au_bw_bookmark_page(w, false);
        return true;
    case AU_CMD_CLOSE_TAB: au_bw_close_tab(w, m->tab); return true;
    case AU_CMD_CLOSE_OTHER_TABS: au_bw_close_other_tabs(w, m->tab); return true;
    case AU_CMD_RELOAD_TAB: au_bw_reload_tab(w, m->tab); return true;
    case AU_CMD_DUPLICATE_TAB: au_bw_duplicate_tab(w, m->tab); return true;
    case AU_CMD_NONE:
    case AU_CMD_SEPARATOR: return true;
    default: return false;
    }
}
