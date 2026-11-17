/* A browser window: a GtkApplicationWindow whose header bar holds Back,
 * Forward, Reload/Stop and the address entry, with the tabs (GtkNotebook),
 * the find bar, the page (a GtkDrawingArea with scroll bars) and the
 * status bar below it (docs/gtk.md, "Windows").
 *
 * The model's window is made with au_browser_window_new_native: its
 * surface holds only the page. The widgets show what the au_bw_*
 * accessors report; AuBrowserHost.changed only records which ones to set
 * (AuGtkWin.pending), and au_gtk_win_flush sets them and paints, so no
 * widget is changed from inside a model call or a widget's own signal. */
#include "gtk.h"

#include <string.h>

static void win_close(AuGtkWin *w);

AuGtkWin *au_gtk_win_of(AuBrowserWindow *bw)
{
    AuGtkWin *w = bw ? bw->platform : NULL;
    return w && !w->freed ? w : NULL;
}

AuGtkWin *au_gtk_front(void)
{
    for (AuGtkWin *w = au_gtk.wins; w; w = w->next)
        if (w == au_gtk.front && !w->closing)
            return w;
    for (AuGtkWin *w = au_gtk.wins; w; w = w->next)
        if (!w->closing)
            return w;
    return NULL;
}

bool au_gtk_win_entry_focused(AuGtkWin *w)
{
    return w->field_focus;
}

/* ---- The page surface ---- */

static void free_surface(AuGtkWin *w)
{
    if (w->surf)
        au_surface_free(w->surf);
    if (w->cs)
        cairo_surface_destroy(w->cs);
    w->surf = NULL;
    w->cs = NULL;
}

/* A new surface for the page area's size, in logical pixels (docs/gtk.md,
 * "HiDPI"), and the model laid out for it. */
static void on_area_resize(GtkDrawingArea *area, int width, int height, gpointer data)
{
    AuGtkWin *w = data;
    (void)area;
    if (width < 1 || height < 1 || w->freed || (width == w->pw && height == w->ph && w->surf))
        return;
    free_surface(w);
    w->cs = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    if (cairo_surface_status(w->cs) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(w->cs);
        w->cs = NULL;
        return;
    }
    unsigned char *px = cairo_image_surface_get_data(w->cs);
    int stride = cairo_image_surface_get_stride(w->cs);
    memset(px, 0xFF, (size_t)stride * (size_t)height);
    cairo_surface_mark_dirty(w->cs);
    w->surf = au_surface_wrap(px, width, height, stride);
    w->pw = width;
    w->ph = height;
    au_present_init(&w->present); /* a pending move was for the old surface */
    au_bw_resize(w->bw, width, height);
    au_present_invalidate(&w->present, (AuIRect){0, 0, width, height});
    w->pending |= AU_CHANGED_SCROLL;
    au_gtk_kick();
}

static void draw_page(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    AuGtkWin *w = data;
    (void)area;
    if (!w->cs) {
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        return;
    }
    /* At a scale factor above 1 the logical pixels are enlarged as they
     * are (docs/gtk.md, "HiDPI"). */
    cairo_set_source_surface(cr, w->cs, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);
}

/* ---- Tabs ---- */

static int page_index(AuGtkWin *w, GtkWidget *child)
{
    return gtk_notebook_page_num(GTK_NOTEBOOK(w->notebook), child);
}

static void on_tab_close(GtkButton *b, gpointer data)
{
    AuGtkWin *w = data;
    GtkWidget *child = g_object_get_data(G_OBJECT(b), "au-page");
    int i = child ? page_index(w, child) : -1;
    if (i >= 0 && !au_gtk.modal)
        au_bw_close_tab(w->bw, i);
    au_gtk_kick();
}

static GtkWidget *tab_label(AuGtkWin *w, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *spinner = gtk_spinner_new();
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(label), 12);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 24);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(close), FALSE);
    gtk_widget_set_focusable(close, FALSE);
    gtk_widget_set_tooltip_text(close, "Close Tab");
    g_object_set_data(G_OBJECT(close), "au-page", child);
    g_signal_connect(close, "clicked", G_CALLBACK(on_tab_close), w);
    gtk_box_append(GTK_BOX(box), spinner);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), close);
    g_object_set_data(G_OBJECT(child), "au-spinner", spinner);
    g_object_set_data(G_OBJECT(child), "au-label", label);
    return box;
}

/* Tabs on top, or a column of tabs on the left (the tab position
 * preference, View > Tabs on Left). */
static void sync_tab_position(AuGtkWin *w)
{
    const AuPrefs *p = &au_gtk.browser->prefs;
    bool left = p->tab_position == AU_TABS_LEFT;
    GtkNotebook *nb = GTK_NOTEBOOK(w->notebook);
    gtk_orientable_set_orientation(GTK_ORIENTABLE(w->body), left ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL);
    gtk_notebook_set_tab_pos(nb, left ? GTK_POS_LEFT : GTK_POS_TOP);
    int width = p->tab_column_width >= 100 ? p->tab_column_width : 180;
    gtk_widget_set_size_request(w->notebook, left ? width : -1, -1);
}

static void sync_tabs(AuGtkWin *w)
{
    GtkNotebook *nb = GTK_NOTEBOOK(w->notebook);
    int n = au_bw_tab_count(w->bw);
    while (gtk_notebook_get_n_pages(nb) > n)
        gtk_notebook_remove_page(nb, gtk_notebook_get_n_pages(nb) - 1);
    while (gtk_notebook_get_n_pages(nb) < n) {
        /* The page widget is empty: the notebook shows only its tabs, and
         * the page is drawn below it. */
        GtkWidget *child = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_notebook_append_page(nb, child, tab_label(w, child));
        gtk_notebook_set_tab_reorderable(nb, child, TRUE);
    }
    for (int i = 0; i < n; i++) {
        GtkWidget *child = gtk_notebook_get_nth_page(nb, i);
        g_object_set_data(G_OBJECT(child), "au-index", GINT_TO_POINTER(i));
        AuChromeTab t;
        if (!au_bw_tab_info(w->bw, i, &t))
            continue;
        const char *title = t.title && *t.title ? t.title : t.url && *t.url ? t.url : "Untitled";
        GtkWidget *label = g_object_get_data(G_OBJECT(child), "au-label");
        GtkWidget *spinner = g_object_get_data(G_OBJECT(child), "au-spinner");
        gtk_label_set_text(GTK_LABEL(label), title);
        gtk_widget_set_sensitive(label, !t.unloaded);
        gtk_widget_set_tooltip_text(gtk_widget_get_parent(label), t.url && *t.url ? t.url : title);
        gtk_spinner_set_spinning(GTK_SPINNER(spinner), t.loading);
        gtk_widget_set_visible(spinner, t.loading);
    }
    int active = au_bw_active_index(w->bw);
    if (active >= 0 && active < n && gtk_notebook_get_current_page(nb) != active)
        gtk_notebook_set_current_page(nb, active);
}

static void on_switch_page(GtkNotebook *nb, GtkWidget *child, guint num, gpointer data)
{
    AuGtkWin *w = data;
    (void)nb;
    (void)child;
    if (w->syncing || w->freed || au_gtk.modal)
        return;
    if ((int)num != au_bw_active_index(w->bw))
        au_bw_select_tab(w->bw, (int)num);
    au_gtk_kick();
}

static void on_page_reordered(GtkNotebook *nb, GtkWidget *child, guint num, gpointer data)
{
    AuGtkWin *w = data;
    (void)nb;
    if (w->syncing || w->freed)
        return;
    int from = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "au-index"));
    if (from != (int)num)
        au_bw_move_tab(w->bw, from, (int)num);
    w->pending |= AU_CHANGED_TABS;
    au_gtk_kick();
}

static void on_new_tab(GtkButton *b, gpointer data)
{
    AuGtkWin *w = data;
    (void)b;
    if (!au_gtk.modal)
        au_bw_open_new_tab(w->bw);
    au_gtk_kick();
}

/* ---- Navigation buttons ---- */

static void on_back(GtkButton *b, gpointer data)
{
    AuGtkWin *w = data;
    (void)b;
    if (!au_gtk.modal)
        au_bw_back(w->bw);
    au_gtk_kick();
}

static void on_forward(GtkButton *b, gpointer data)
{
    AuGtkWin *w = data;
    (void)b;
    if (!au_gtk.modal)
        au_bw_forward(w->bw);
    au_gtk_kick();
}

static void on_reload(GtkButton *b, gpointer data)
{
    AuGtkWin *w = data;
    if (au_gtk.modal)
        return;
    /* Shift reloads past the cache, as Reload in the menu does. */
    au_bw_reload_or_stop(w->bw, (au_gtk_current_mods(GTK_WIDGET(b)) & AU_MOD_SHIFT) != 0);
    au_gtk_kick();
}

static void sync_nav(AuGtkWin *w)
{
    gtk_widget_set_sensitive(w->back, au_bw_can_go(w->bw, -1));
    gtk_widget_set_sensitive(w->forward, au_bw_can_go(w->bw, 1));
    bool loading = au_bw_loading(w->bw);
    gtk_button_set_icon_name(GTK_BUTTON(w->reload), loading ? "process-stop-symbolic" : "view-refresh-symbolic");
    gtk_widget_set_tooltip_text(w->reload, loading ? "Stop" : "Reload");
}

/* ---- The address entry ----
 * Typing sets the model's field text (au_bw_set_field_text). When the
 * text grows at its end, the completion from history and bookmarks
 * (au_bw_complete_field) fills the rest, selected, so the next key
 * replaces it. */

static long char_len(const char *s)
{
    return g_utf8_strlen(s, -1);
}

static gboolean complete_idle(gpointer data)
{
    AuGtkWin *w = data;
    w->complete_id = 0;
    if (w->freed || !w->typed || !*w->typed)
        return G_SOURCE_REMOVE;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(w->entry));
    if (strcmp(text, w->typed))
        return G_SOURCE_REMOVE;
    AuBuf out;
    au_buf_init(&out);
    size_t tl = strlen(w->typed);
    if (au_bw_complete_field(w->bw, w->typed, &out) && out.len > tl && !strncmp(out.data, w->typed, tl)) {
        char *full = au_buf_cstr(&out);
        w->syncing = true;
        gtk_editable_set_text(GTK_EDITABLE(w->entry), full);
        gtk_editable_select_region(GTK_EDITABLE(w->entry), (int)char_len(w->typed), -1);
        w->syncing = false;
        au_bw_set_field_text(w->bw, full);
    }
    au_buf_free(&out);
    return G_SOURCE_REMOVE;
}

static void on_entry_changed(GtkEditable *e, gpointer data)
{
    AuGtkWin *w = data;
    if (w->syncing || w->freed)
        return;
    const char *text = gtk_editable_get_text(e);
    bool grew = w->typed ? strlen(text) > strlen(w->typed) && !strncmp(text, w->typed, strlen(w->typed))
                         : *text != 0;
    bool at_end = gtk_editable_get_position(e) >= char_len(text) - 1;
    g_free(w->typed);
    w->typed = g_strdup(text);
    au_bw_set_field_text(w->bw, text);
    if (w->complete_id) {
        g_source_remove(w->complete_id);
        w->complete_id = 0;
    }
    if (grew && at_end)
        w->complete_id = g_idle_add(complete_idle, w);
}

static void on_entry_activate(GtkEntry *e, gpointer data)
{
    AuGtkWin *w = data;
    if (au_gtk.modal)
        return;
    char *text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(e)));
    g_free(w->typed);
    w->typed = NULL;
    gtk_widget_grab_focus(w->area);
    au_bw_navigate_input(w->bw, text);
    g_free(text);
    au_gtk_kick();
}

static gboolean on_entry_key(GtkEventControllerKey *k, guint keyval, guint code, GdkModifierType state, gpointer data)
{
    AuGtkWin *w = data;
    (void)k;
    (void)code;
    (void)state;
    if (keyval != GDK_KEY_Escape)
        return FALSE;
    /* Escape puts back the page's address. */
    g_free(w->typed);
    w->typed = NULL;
    au_bw_set_field_text(w->bw, NULL);
    au_gtk_kick();
    return TRUE;
}

static void on_entry_focus_in(GtkEventControllerFocus *f, gpointer data)
{
    AuGtkWin *w = data;
    (void)f;
    w->field_focus = true;
    if (!w->freed)
        au_bw_set_field_focus(w->bw, true);
}

static void on_entry_focus_out(GtkEventControllerFocus *f, gpointer data)
{
    AuGtkWin *w = data;
    (void)f;
    w->field_focus = false;
    if (!w->freed)
        au_bw_set_field_focus(w->bw, false);
}

static void sync_field(AuGtkWin *w)
{
    /* Leave the text alone while the user edits it. */
    if (w->field_focus && w->typed && au_bw_typing(w->bw))
        return;
    const char *text = au_bw_field_text(w->bw);
    if (!text)
        text = "";
    if (!strcmp(text, gtk_editable_get_text(GTK_EDITABLE(w->entry))))
        return;
    w->syncing = true;
    gtk_editable_set_text(GTK_EDITABLE(w->entry), text);
    w->syncing = false;
    g_free(w->typed);
    w->typed = NULL;
}

static void sync_security(AuGtkWin *w)
{
    const char *icon = NULL, *tip = NULL;
    switch (au_bw_security(w->bw)) {
    case AU_SECURITY_SECURE: icon = "channel-secure-symbolic"; tip = "Secure connection"; break;
    case AU_SECURITY_NOT_SECURE: icon = "channel-insecure-symbolic"; tip = "Not secure"; break;
    case AU_SECURITY_CERT_ERROR: icon = "dialog-warning-symbolic"; tip = "Certificate problem"; break;
    case AU_SECURITY_MIXED: icon = "channel-insecure-symbolic"; tip = "Partly secure"; break;
    default: break;
    }
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(w->entry), GTK_ENTRY_ICON_PRIMARY, icon);
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(w->entry), GTK_ENTRY_ICON_PRIMARY, tip);
}

void au_gtk_win_focus_address(AuGtkWin *w)
{
    gtk_widget_grab_focus(w->entry);
    gtk_editable_select_region(GTK_EDITABLE(w->entry), 0, -1);
}

/* ---- Scroll bars ---- */

static void on_scroll_value(GtkAdjustment *adj, gpointer data)
{
    AuGtkWin *w = data;
    (void)adj;
    if (w->syncing || w->freed || au_gtk.modal)
        return;
    au_bw_scroll_to(w->bw, (int)gtk_adjustment_get_value(w->hadj), (int)gtk_adjustment_get_value(w->vadj));
    au_gtk_kick();
}

static void sync_adj(GtkAdjustment *a, int pos, int max, int page)
{
    if (page < 1)
        page = 1;
    gtk_adjustment_configure(a, pos, 0, max + page, 40, page > 80 ? page - 40 : page, page);
}

static void sync_scroll(AuGtkWin *w)
{
    int x, y, mx, my;
    au_bw_scroll_pos(w->bw, &x, &y);
    au_bw_scroll_max(w->bw, &mx, &my);
    w->syncing = true;
    sync_adj(w->hadj, x, mx, w->pw);
    sync_adj(w->vadj, y, my, w->ph);
    w->syncing = false;
    /* The vertical bar stays (the page width does not jump as pages
     * load); the horizontal one shows only when the page is wider. */
    gtk_widget_set_sensitive(w->vscroll, my > 0);
    gtk_widget_set_visible(w->hscroll, mx > 0);
}

/* ---- Find ---- */

static void on_find_activate(GtkSearchEntry *e, gpointer data)
{
    AuGtkWin *w = data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(e));
    if (!*text || au_gtk.modal)
        return;
    if (!au_bw_find(w->bw, text, false))
        gtk_widget_error_bell(GTK_WIDGET(e));
    au_gtk_kick();
}

static void find_again(AuGtkWin *w, bool forward)
{
    if (au_gtk.modal)
        return;
    if (!au_bw_find_again(w->bw, forward))
        gtk_widget_error_bell(w->search_entry);
    au_gtk_kick();
}

static void on_find_next(GtkSearchEntry *e, gpointer data)
{
    (void)e;
    find_again(data, true);
}

static void on_find_previous(GtkSearchEntry *e, gpointer data)
{
    (void)e;
    find_again(data, false);
}

static void on_find_stop(GtkSearchEntry *e, gpointer data)
{
    AuGtkWin *w = data;
    (void)e;
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(w->search_bar), FALSE);
    gtk_widget_grab_focus(w->area);
}

void au_gtk_win_find(AuGtkWin *w)
{
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(w->search_bar), TRUE);
    gtk_widget_grab_focus(w->search_entry);
    gtk_editable_select_region(GTK_EDITABLE(w->search_entry), 0, -1);
}

/* ---- The window ---- */

static void on_active(GObject *obj, GParamSpec *ps, gpointer data)
{
    AuGtkWin *w = data;
    (void)ps;
    if (w->freed)
        return;
    bool active = gtk_window_is_active(GTK_WINDOW(obj));
    au_bw_set_active(w->bw, active);
    if (active) {
        au_gtk.front = w;
        au_gtk_menus_dirty();
    }
}

static gboolean on_close_request(GtkWindow *win, gpointer data)
{
    AuGtkWin *w = data;
    (void)win;
    if (au_gtk.modal)
        return TRUE;
    win_close(w);
    return TRUE;
}

/* The window manager's close: the last window quits, as on other
 * GTK applications; another one closes with its tabs. */
static void win_close(AuGtkWin *w)
{
    int open = 0;
    for (AuGtkWin *o = au_gtk.wins; o; o = o->next)
        if (!o->closing)
            open++;
    if (open <= 1)
        au_gtk_quit();
    else
        au_gtk_win_destroy(w);
}

static GtkWidget *nav_button(const char *icon, const char *tip, GCallback cb, AuGtkWin *w)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_focusable(b, FALSE);
    g_signal_connect(b, "clicked", cb, w);
    return b;
}

AuGtkWin *au_gtk_win_new(int width, int height)
{
    AuGtkWin *w = g_new0(AuGtkWin, 1);
    w->bw = au_browser_window_new_native(au_gtk.browser, width, height);
    if (!w->bw) {
        g_free(w);
        return NULL;
    }
    w->bw->platform = w;
    w->bw->native_field = true;
    au_present_init(&w->present);

    GtkWidget *win = gtk_application_window_new(au_gtk.app);
    w->window = GTK_WINDOW(win);
    gtk_application_window_set_show_menubar(GTK_APPLICATION_WINDOW(win), TRUE);
    /* The page area's size, plus the header bar, tabs and status bar. */
    gtk_window_set_default_size(w->window, width, height + 110);
    gtk_window_set_title(w->window, "Aura");

    GtkWidget *hb = gtk_header_bar_new();
    w->back = nav_button("go-previous-symbolic", "Back", G_CALLBACK(on_back), w);
    w->forward = nav_button("go-next-symbolic", "Forward", G_CALLBACK(on_forward), w);
    w->reload = nav_button("view-refresh-symbolic", "Reload", G_CALLBACK(on_reload), w);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), w->back);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), w->forward);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), w->reload);
    w->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->entry), "Search or enter address");
    gtk_entry_set_input_purpose(GTK_ENTRY(w->entry), GTK_INPUT_PURPOSE_URL);
    gtk_widget_set_hexpand(w->entry, TRUE);
    gtk_widget_set_size_request(w->entry, 320, -1);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb), w->entry);
    g_signal_connect(w->entry, "changed", G_CALLBACK(on_entry_changed), w);
    g_signal_connect(w->entry, "activate", G_CALLBACK(on_entry_activate), w);
    GtkEventController *ek = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(ek, GTK_PHASE_CAPTURE);
    g_signal_connect(ek, "key-pressed", G_CALLBACK(on_entry_key), w);
    gtk_widget_add_controller(w->entry, ek);
    GtkEventController *ef = gtk_event_controller_focus_new();
    g_signal_connect(ef, "enter", G_CALLBACK(on_entry_focus_in), w);
    g_signal_connect(ef, "leave", G_CALLBACK(on_entry_focus_out), w);
    gtk_widget_add_controller(w->entry, ef);
    gtk_window_set_titlebar(w->window, hb);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    w->notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(w->notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(w->notebook), FALSE);
    gtk_widget_set_focusable(w->notebook, FALSE);
    GtkWidget *plus = gtk_button_new_from_icon_name("tab-new-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(plus), FALSE);
    gtk_widget_set_tooltip_text(plus, "New Tab");
    gtk_widget_set_focusable(plus, FALSE);
    g_signal_connect(plus, "clicked", G_CALLBACK(on_new_tab), w);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(w->notebook), plus, GTK_PACK_END);
    g_signal_connect(w->notebook, "switch-page", G_CALLBACK(on_switch_page), w);
    g_signal_connect(w->notebook, "page-reordered", G_CALLBACK(on_page_reordered), w);
    /* The tabs and the page share a box that turns horizontal for tabs
     * on the left (sync_tab_position). */
    w->body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(w->body, TRUE);
    gtk_box_append(GTK_BOX(box), w->body);
    gtk_box_append(GTK_BOX(w->body), w->notebook);
    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(column, TRUE);
    gtk_widget_set_vexpand(column, TRUE);
    gtk_box_append(GTK_BOX(w->body), column);

    w->search_bar = gtk_search_bar_new();
    w->search_entry = gtk_search_entry_new();
    gtk_widget_set_size_request(w->search_entry, 300, -1);
    gtk_search_bar_set_child(GTK_SEARCH_BAR(w->search_bar), w->search_entry);
    gtk_search_bar_connect_entry(GTK_SEARCH_BAR(w->search_bar), GTK_EDITABLE(w->search_entry));
    gtk_search_bar_set_show_close_button(GTK_SEARCH_BAR(w->search_bar), TRUE);
    g_signal_connect(w->search_entry, "activate", G_CALLBACK(on_find_activate), w);
    g_signal_connect(w->search_entry, "next-match", G_CALLBACK(on_find_next), w);
    g_signal_connect(w->search_entry, "previous-match", G_CALLBACK(on_find_previous), w);
    g_signal_connect(w->search_entry, "stop-search", G_CALLBACK(on_find_stop), w);
    gtk_box_append(GTK_BOX(column), w->search_bar);

    GtkWidget *grid = gtk_grid_new();
    w->area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(w->area, TRUE);
    gtk_widget_set_vexpand(w->area, TRUE);
    gtk_widget_set_focusable(w->area, TRUE);
    gtk_widget_set_cursor_from_name(w->area, "default");
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(w->area), draw_page, w, NULL);
    g_signal_connect(w->area, "resize", G_CALLBACK(on_area_resize), w);
    w->hadj = gtk_adjustment_new(0, 0, 1, 40, 100, 1);
    w->vadj = gtk_adjustment_new(0, 0, 1, 40, 100, 1);
    g_signal_connect(w->hadj, "value-changed", G_CALLBACK(on_scroll_value), w);
    g_signal_connect(w->vadj, "value-changed", G_CALLBACK(on_scroll_value), w);
    w->vscroll = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, w->vadj);
    w->hscroll = gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, w->hadj);
    gtk_widget_set_visible(w->hscroll, FALSE);
    gtk_grid_attach(GTK_GRID(grid), w->area, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), w->vscroll, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), w->hscroll, 0, 1, 1, 1);
    gtk_box_append(GTK_BOX(column), grid);

    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(status, 6);
    gtk_widget_set_margin_end(status, 6);
    gtk_widget_set_margin_top(status, 2);
    gtk_widget_set_margin_bottom(status, 2);
    w->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(w->status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(w->status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_single_line_mode(GTK_LABEL(w->status), TRUE);
    gtk_widget_set_hexpand(w->status, TRUE);
    gtk_widget_add_css_class(w->status, "caption");
    w->progress = gtk_label_new("");
    gtk_widget_add_css_class(w->progress, "caption");
    gtk_widget_add_css_class(w->progress, "dim-label");
    gtk_box_append(GTK_BOX(status), w->status);
    gtk_box_append(GTK_BOX(status), w->progress);
    gtk_box_append(GTK_BOX(box), status);

    gtk_window_set_child(w->window, box);
    g_signal_connect(win, "close-request", G_CALLBACK(on_close_request), w);
    g_signal_connect(win, "notify::is-active", G_CALLBACK(on_active), w);
    au_gtk_input_attach(w);

    w->next = au_gtk.wins;
    au_gtk.wins = w;
    au_gtk.front = w;
    w->pending = ~0u & ~(unsigned)(AU_CHANGED_FOCUS_FIELD | AU_CHANGED_FOCUS_PAGE);
    au_gtk_menus_dirty();
    gtk_widget_grab_focus(w->area);
    return w;
}

static gboolean free_later(gpointer data)
{
    g_free(data);
    return G_SOURCE_REMOVE;
}

void au_gtk_win_destroy(AuGtkWin *w)
{
    AuGtkWin **pp = &au_gtk.wins;
    while (*pp && *pp != w)
        pp = &(*pp)->next;
    if (*pp)
        *pp = w->next;
    if (au_gtk.front == w)
        au_gtk.front = NULL;
    if (w->complete_id)
        g_source_remove(w->complete_id);
    w->complete_id = 0;
    if (w->have_menu)
        au_context_menu_free(&w->menu);
    w->have_menu = false;
    if (w->menu_popover)
        gtk_widget_unparent(w->menu_popover);
    w->menu_popover = NULL;
    AuBrowserWindow *bw = w->bw;
    w->freed = true;
    w->bw = NULL;
    bw->platform = NULL;
    au_browser_window_free(bw);
    free_surface(w);
    g_free(w->typed);
    /* The GtkWindow's signal handlers still point at w: disconnect them
     * before the window goes. */
    g_signal_handlers_disconnect_by_data(w->window, w);
    g_signal_handlers_disconnect_by_data(w->hadj, w);
    g_signal_handlers_disconnect_by_data(w->vadj, w);
    g_signal_handlers_disconnect_by_data(w->notebook, w);
    gtk_window_destroy(w->window);
    /* Controllers inside the window may still run a handler while it is
     * torn down; they see w->freed. */
    g_idle_add(free_later, w);
    au_gtk_menus_dirty();
}

void au_gtk_win_changed(AuGtkWin *w, unsigned what)
{
    /* The later of two focus requests in one batch wins: a new tab asks
     * for the field while blank, then for the page once its load starts. */
    if (what & (AU_CHANGED_FOCUS_FIELD | AU_CHANGED_FOCUS_PAGE))
        w->pending &= ~(unsigned)(AU_CHANGED_FOCUS_FIELD | AU_CHANGED_FOCUS_PAGE);
    w->pending |= what;
    au_gtk_kick();
}

static void apply_changes(AuGtkWin *w)
{
    unsigned what = w->pending;
    w->pending = 0;
    if (!what)
        return;
    if (what & AU_CHANGED_TITLE) {
        char title[256];
        au_bw_window_title(w->bw, title, sizeof title);
        gtk_window_set_title(w->window, *title ? title : "Aura");
        au_gtk_menus_dirty(); /* the Window menu lists the titles */
    }
    if (what & AU_CHANGED_LAYOUT)
        sync_tab_position(w);
    if (what & (AU_CHANGED_TABS | AU_CHANGED_TAB_INFO | AU_CHANGED_ACTIVE_TAB)) {
        w->syncing = true;
        sync_tabs(w);
        w->syncing = false;
    }
    if (what & (AU_CHANGED_NAV | AU_CHANGED_ACTIVE_TAB | AU_CHANGED_TABS))
        sync_nav(w);
    if (what & (AU_CHANGED_FIELD | AU_CHANGED_ACTIVE_TAB))
        sync_field(w);
    if (what & (AU_CHANGED_SECURITY | AU_CHANGED_ACTIVE_TAB))
        sync_security(w);
    if (what & (AU_CHANGED_SCROLL | AU_CHANGED_LAYOUT | AU_CHANGED_ACTIVE_TAB))
        sync_scroll(w);
    if (what & (AU_CHANGED_STATUS | AU_CHANGED_ACTIVE_TAB)) {
        char s[512];
        au_bw_status_text(w->bw, s, sizeof s);
        gtk_label_set_text(GTK_LABEL(w->status), s);
    }
    if (what & (AU_CHANGED_PROGRESS | AU_CHANGED_NAV | AU_CHANGED_ACTIVE_TAB)) {
        char s[128];
        au_bw_progress_text(w->bw, s, sizeof s);
        gtk_label_set_text(GTK_LABEL(w->progress), s);
        gtk_entry_set_progress_fraction(GTK_ENTRY(w->entry), au_bw_loading(w->bw) ? au_bw_progress(w->bw) : 0);
    }
    if (what & (AU_CHANGED_MENUS | AU_CHANGED_BOOKMARK))
        au_gtk_menus_dirty();
    if (what & AU_CHANGED_FOCUS_FIELD)
        au_gtk_win_focus_address(w);
    else if (what & AU_CHANGED_FOCUS_PAGE)
        gtk_widget_grab_focus(w->area);
}

void au_gtk_win_flush(AuGtkWin *w)
{
    if (w->freed)
        return;
    apply_changes(w);
    if (!au_present_pending(&w->present) || !w->surf)
        return;
    cairo_surface_flush(w->cs);
    AuPresentRound r;
    au_present_begin(&w->present, w->surf, &r);
    AuCanvas *c = au_canvas_new(w->surf);
    for (int i = 0; i < r.n; i++)
        au_bw_paint(w->bw, c, r.rects[i]);
    au_canvas_free(c);
    cairo_surface_mark_dirty(w->cs);
    gtk_widget_queue_draw(w->area);
}
