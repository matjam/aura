/* The Bookmarks, History, Downloads and Cookies windows.
 *
 * The rows, the selection, the buttons, what each action does and when
 * the list is stale are the model in src/browser/lists.c. This file holds
 * only what is about GTK:
 *   - a GtkColumnView over a GListStore with one plain GObject per model
 *     row (its index is kept on the object), rebuilt when the model's
 *     generation moves, with the columns and buttons the model names;
 *   - the first column's indent, disclosure button and icon; a progress
 *     bar in the Downloads window's Progress column;
 *   - a 250 ms timer that refreshes a stale model;
 *   - the AuListHost callbacks: the caller's open_url and
 *     reveal_download, and the nested dialogs of winutil_gtk.c. */
#include "winutil_gtk.h"

#include <string.h>

typedef struct ListWin {
    AuListModel m;
    GtkWindow *win;
    GListStore *store;
    GtkSingleSelection *sel;
    GtkWidget *view;
    GtkWidget *buttons[AU_LIST_ACTIONS];
    AuListAction actions[AU_LIST_ACTIONS];
    int nbuttons;
    uint32_t shown_gen; /* the generation the store holds */
    bool shown;         /* the store was filled once */
    bool syncing;       /* the selection is being set from the model */
    guint timer;
} ListWin;

static ListWin *g_lists[AU_LIST_KINDS];

/* A column's factory data. */
typedef struct Col {
    ListWin *lw;
    int col;
} Col;

static GQuark row_quark(void)
{
    static GQuark q;
    if (!q)
        q = g_quark_from_static_string("aura-list-row");
    return q;
}

static int row_of(GObject *o) { return GPOINTER_TO_INT(g_object_get_qdata(o, row_quark())) - 1; }

/* ---- Keeping the view in step with the model ---- */

static void enable_buttons(ListWin *lw)
{
    for (int i = 0; i < lw->nbuttons; i++)
        gtk_widget_set_sensitive(lw->buttons[i], au_list_can(&lw->m, lw->actions[i]));
}

static void sync_view(ListWin *lw)
{
    lw->syncing = true;
    if (!lw->shown || lw->shown_gen != lw->m.generation) {
        guint old = g_list_model_get_n_items(G_LIST_MODEL(lw->store));
        GObject **rows = g_new(GObject *, lw->m.n > 0 ? lw->m.n : 1);
        for (int i = 0; i < lw->m.n; i++) {
            rows[i] = g_object_new(G_TYPE_OBJECT, NULL);
            g_object_set_qdata(rows[i], row_quark(), GINT_TO_POINTER(i + 1));
        }
        g_list_store_splice(lw->store, 0, old, (gpointer *)rows, (guint)lw->m.n);
        for (int i = 0; i < lw->m.n; i++)
            g_object_unref(rows[i]);
        g_free(rows);
        lw->shown_gen = lw->m.generation;
        lw->shown = true;
    }
    guint want = lw->m.selected >= 0 ? (guint)lw->m.selected : GTK_INVALID_LIST_POSITION;
    if (gtk_single_selection_get_selected(lw->sel) != want)
        gtk_single_selection_set_selected(lw->sel, want);
    lw->syncing = false;
    enable_buttons(lw);
}

static gboolean tick(gpointer data)
{
    ListWin *lw = data;
    if (au_gtk_dialog_running())
        return G_SOURCE_CONTINUE;
    if (au_list_stale(&lw->m)) {
        au_list_refresh(&lw->m, au_gtk_now());
        sync_view(lw);
    }
    return G_SOURCE_CONTINUE;
}

static void run_action(ListWin *lw, AuListAction a)
{
    if (au_list_run(&lw->m, a, au_gtk_now()) == AU_LIST_FAILED)
        gtk_widget_error_bell(GTK_WIDGET(lw->win));
    if (g_lists[lw->m.kind] == lw)
        sync_view(lw);
}

/* ---- The model's callbacks ---- */

static void h_open_url(void *ctx, const char *url, bool new_tab)
{
    (void)ctx;
    if (au_gtk_host.open_url)
        au_gtk_host.open_url(au_gtk_host.ctx, url, new_tab);
}

static char *h_ask_text(void *ctx, const char *prompt, const char *initial)
{
    ListWin *lw = ctx;
    return au_gtk_ask_text(lw->win, au_list_title(lw->m.kind), prompt, initial);
}

static void h_show_text(void *ctx, const char *title, const char *text)
{
    ListWin *lw = ctx;
    au_gtk_show_text(lw->win, title, text);
}

static int h_choose(void *ctx, AuListAction action, const char *const *items, int n, int current)
{
    ListWin *lw = ctx;
    const char *title = au_list_title(lw->m.kind);
    const AuListButton *b;
    int nb = au_list_buttons(lw->m.kind, &b);
    for (int i = 0; i < nb; i++)
        if (b[i].action == action)
            title = b[i].label;
    return au_gtk_choose(lw->win, title, NULL, items, n, current);
}

static bool h_reveal(void *ctx, const AuDownload *d)
{
    (void)ctx;
    return au_gtk_host.reveal_download && au_gtk_host.reveal_download(au_gtk_host.ctx, d);
}

static void h_rules_changed(void *ctx)
{
    (void)ctx;
    if (au_gtk_host.aura_app)
        au_app_cookie_rules_changed(au_gtk_host.aura_app);
}

/* ---- Cells ---- */

static const char *icon_name(AuListIcon icon)
{
    switch (icon) {
    case AU_LIST_ICON_FOLDER: return "folder-symbolic";
    case AU_LIST_ICON_PAGE: return "text-html-symbolic";
    case AU_LIST_ICON_FILE: return "text-x-generic-symbolic";
    case AU_LIST_ICON_COOKIE: return "preferences-system-privacy-symbolic";
    case AU_LIST_ICON_STORAGE: return "drive-harddisk-symbolic";
    default: return NULL;
    }
}

static void disclosure_clicked(GtkButton *b, gpointer data)
{
    ListWin *lw = data;
    int row = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "aura-row")) - 1;
    if (row < 0 || row >= lw->m.n || au_gtk_dialog_running())
        return;
    au_list_toggle(&lw->m, row, au_gtk_now());
    sync_view(lw);
}

/* The first column: [indent][disclosure][icon] text. */
static void setup_first(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data)
{
    Col *c = data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *disc = gtk_button_new_from_icon_name("pan-end-symbolic");
    gtk_widget_add_css_class(disc, "flat");
    gtk_widget_set_focusable(disc, FALSE);
    gtk_widget_set_valign(disc, GTK_ALIGN_CENTER);
    g_signal_connect(disc, "clicked", G_CALLBACK(disclosure_clicked), c->lw);
    GtkWidget *icon = gtk_image_new();
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), disc);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_item_set_child(li, box);
}

static void bind_first(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data)
{
    Col *c = data;
    int row = row_of(gtk_list_item_get_item(li));
    if (row < 0 || row >= c->lw->m.n)
        return;
    const AuListItem *it = &c->lw->m.items[row];
    GtkWidget *box = gtk_list_item_get_child(li);
    GtkWidget *disc = gtk_widget_get_first_child(box);
    GtkWidget *icon = gtk_widget_get_next_sibling(disc);
    GtkWidget *label = gtk_widget_get_next_sibling(icon);
    gtk_widget_set_margin_start(box, it->depth * 18);
    bool folder = it->type == AU_LIST_ITEM_FOLDER;
    gtk_widget_set_visible(disc, folder);
    if (folder)
        gtk_button_set_icon_name(GTK_BUTTON(disc), it->expanded ? "pan-down-symbolic" : "pan-end-symbolic");
    g_object_set_data(G_OBJECT(disc), "aura-row", GINT_TO_POINTER(row + 1));
    const char *name = icon_name(it->icon);
    gtk_widget_set_visible(icon, name != NULL);
    if (name)
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), name);
    gtk_label_set_text(GTK_LABEL(label), it->cols[c->col]);
    if (it->type == AU_LIST_ITEM_GROUP)
        gtk_widget_add_css_class(label, "heading");
    else
        gtk_widget_remove_css_class(label, "heading");
    gtk_widget_set_tooltip_text(box, it->summary && it->summary[0] ? it->summary : NULL);
}

static void setup_text(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data)
{
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_list_item_set_child(li, label);
}

static void bind_text(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data)
{
    Col *c = data;
    int row = row_of(gtk_list_item_get_item(li));
    if (row < 0 || row >= c->lw->m.n)
        return;
    const char *t = c->lw->m.items[row].cols[c->col];
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), t);
    gtk_widget_set_tooltip_text(gtk_list_item_get_child(li), t[0] ? t : NULL);
}

/* Downloads' Progress column: a bar while the size is known, and the
 * model's text. */
static void setup_progress(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *bar = gtk_progress_bar_new();
    gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(bar, 60, -1);
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_item_set_child(li, box);
}

static void bind_progress(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data)
{
    Col *c = data;
    int row = row_of(gtk_list_item_get_item(li));
    if (row < 0 || row >= c->lw->m.n)
        return;
    const AuListItem *it = &c->lw->m.items[row];
    GtkWidget *box = gtk_list_item_get_child(li);
    GtkWidget *bar = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_next_sibling(bar);
    bool show = it->progress >= 0 && (it->state == AU_DL_ACTIVE || it->state == AU_DL_PAUSED ||
                                      it->state == AU_DL_QUEUED);
    gtk_widget_set_visible(bar, show);
    if (show)
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), it->progress / 1000.0);
    gtk_label_set_text(GTK_LABEL(label), it->cols[c->col]);
}

static void add_column(ListWin *lw, int col, const char *heading)
{
    GtkListItemFactory *f = gtk_signal_list_item_factory_new();
    Col *c = g_new(Col, 1);
    c->lw = lw;
    c->col = col;
    g_object_set_data_full(G_OBJECT(f), "aura-col", c, g_free);
    if (col == 0) {
        g_signal_connect(f, "setup", G_CALLBACK(setup_first), c);
        g_signal_connect(f, "bind", G_CALLBACK(bind_first), c);
    } else if (lw->m.kind == AU_LIST_DOWNLOADS && col == 3) {
        g_signal_connect(f, "setup", G_CALLBACK(setup_progress), c);
        g_signal_connect(f, "bind", G_CALLBACK(bind_progress), c);
    } else {
        g_signal_connect(f, "setup", G_CALLBACK(setup_text), c);
        g_signal_connect(f, "bind", G_CALLBACK(bind_text), c);
    }
    GtkColumnViewColumn *cv = gtk_column_view_column_new(heading, f);
    gtk_column_view_column_set_resizable(cv, TRUE);
    if (col == 0) {
        gtk_column_view_column_set_expand(cv, TRUE);
        gtk_column_view_column_set_fixed_width(cv, 220);
    }
    gtk_column_view_append_column(GTK_COLUMN_VIEW(lw->view), cv);
    g_object_unref(cv);
}

/* ---- Events ---- */

static void selected_changed(GtkSingleSelection *s, GParamSpec *p, gpointer data)
{
    ListWin *lw = data;
    if (lw->syncing)
        return;
    guint pos = gtk_single_selection_get_selected(s);
    au_list_select(&lw->m, pos == GTK_INVALID_LIST_POSITION ? -1 : (int)pos);
    sync_view(lw); /* a day heading is not selected */
}

static void activated(GtkColumnView *v, guint pos, gpointer data)
{
    ListWin *lw = data;
    if (au_gtk_dialog_running())
        return;
    if (au_list_activate(&lw->m, (int)pos, au_gtk_now()) == AU_LIST_FAILED)
        gtk_widget_error_bell(GTK_WIDGET(lw->win));
    if (g_lists[lw->m.kind] == lw)
        sync_view(lw);
}

static void button_clicked(GtkButton *b, gpointer data)
{
    ListWin *lw = data;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "aura-button"));
    if (!au_gtk_dialog_running())
        run_action(lw, lw->actions[i]);
}

static gboolean key_pressed(GtkEventControllerKey *k, guint keyval, guint code, GdkModifierType state, gpointer data)
{
    ListWin *lw = data;
    if (keyval == GDK_KEY_Delete || keyval == GDK_KEY_KP_Delete || keyval == GDK_KEY_BackSpace) {
        if (au_list_can(&lw->m, AU_LIST_DELETE))
            run_action(lw, AU_LIST_DELETE);
        else
            gtk_widget_error_bell(GTK_WIDGET(lw->win));
        return TRUE;
    }
    return FALSE;
}

static gboolean close_request(GtkWindow *w, gpointer data)
{
    (void)w;
    (void)data;
    return au_gtk_dialog_running(); /* the model is waiting on the dialog */
}

static void destroyed(GtkWidget *w, gpointer data)
{
    ListWin *lw = data;
    if (lw->timer)
        g_source_remove(lw->timer);
    g_lists[lw->m.kind] = NULL;
    g_signal_handlers_disconnect_by_data(lw->sel, lw);
    g_object_unref(lw->sel);
    au_list_free(&lw->m);
    g_object_unref(lw->store);
    g_free(lw);
    au_gtk_windows_changed();
}

void au_gtk_list_show(AuListKind kind)
{
    if (kind < 0 || kind >= AU_LIST_KINDS || !au_gtk_host.browser)
        return;
    if (g_lists[kind]) {
        gtk_window_present(g_lists[kind]->win);
        return;
    }
    au_gtk_window_css();
    ListWin *lw = g_new0(ListWin, 1);
    AuListSources src;
    AuApp *app = au_gtk_host.aura_app;
    au_list_sources(&src, au_gtk_host.browser, app ? au_app_cookie_jar(app) : NULL, app ? au_app_storage(app) : NULL);
    AuListHost host = {lw, h_open_url, h_ask_text, h_show_text, h_choose, h_reveal, h_rules_changed};
    au_list_init(&lw->m, kind, &src, &host, au_gtk_now());

    GtkWidget *w = gtk_window_new();
    lw->win = GTK_WINDOW(w);
    gtk_window_set_title(lw->win, au_list_title(kind));
    gtk_window_set_default_size(lw->win, kind == AU_LIST_DOWNLOADS || kind == AU_LIST_COOKIES ? 720 : 640, 480);
    if (au_gtk_host.app)
        gtk_window_set_application(lw->win, au_gtk_host.app);

    lw->store = g_list_store_new(G_TYPE_OBJECT);
    lw->sel = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(lw->store)));
    gtk_single_selection_set_autoselect(lw->sel, FALSE);
    gtk_single_selection_set_can_unselect(lw->sel, TRUE);
    /* The view takes the selection; the window keeps its own reference
     * so destroyed() can still disconnect from it after the view goes. */
    lw->view = gtk_column_view_new(GTK_SELECTION_MODEL(g_object_ref(lw->sel)));
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(lw->view), TRUE);
    gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(lw->view), FALSE);
    const char *const *headings;
    int ncols = au_list_columns(kind, &headings);
    for (int i = 0; i < ncols; i++)
        add_column(lw, i, headings[i]);
    g_signal_connect(lw->sel, "notify::selected", G_CALLBACK(selected_changed), lw);
    g_signal_connect(lw->view, "activate", G_CALLBACK(activated), lw);
    GtkEventController *k = gtk_event_controller_key_new();
    g_signal_connect(k, "key-pressed", G_CALLBACK(key_pressed), lw);
    gtk_widget_add_controller(lw->view, k);

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), lw->view);
    gtk_widget_set_vexpand(sw, TRUE);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 8);
    gtk_widget_set_margin_bottom(bar, 8);
    const AuListButton *b;
    int nb = au_list_buttons(kind, &b);
    for (int i = 0; i < nb && i < AU_LIST_ACTIONS; i++) {
        GtkWidget *btn = gtk_button_new_with_label(b[i].label);
        g_object_set_data(G_OBJECT(btn), "aura-button", GINT_TO_POINTER(i));
        g_signal_connect(btn, "clicked", G_CALLBACK(button_clicked), lw);
        gtk_box_append(GTK_BOX(bar), btn);
        lw->buttons[i] = btn;
        lw->actions[i] = b[i].action;
        lw->nbuttons++;
    }
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), sw);
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box), bar);
    gtk_window_set_child(lw->win, box);

    g_signal_connect(w, "close-request", G_CALLBACK(close_request), lw);
    g_signal_connect(w, "destroy", G_CALLBACK(destroyed), lw);
    g_lists[kind] = lw;
    sync_view(lw);
    lw->timer = g_timeout_add(250, tick, lw);
    gtk_window_present(lw->win);
    gtk_widget_grab_focus(lw->view);
    au_gtk_windows_changed();
}

void au_gtk_list_touch(AuListKind kind)
{
    if (kind >= 0 && kind < AU_LIST_KINDS && g_lists[kind])
        au_list_touch(&g_lists[kind]->m);
}

void au_gtk_lists_close(void)
{
    for (int i = 0; i < AU_LIST_KINDS; i++)
        if (g_lists[i])
            gtk_window_destroy(g_lists[i]->win);
}
