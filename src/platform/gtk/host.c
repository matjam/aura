/* AuBrowserHost for the GTK4 port, and the dialogs the model waits for.
 *
 * The model asks some questions synchronously (a select's pop-up menu, a
 * confirmation, files for an input type=file, a download warning). GTK4's
 * dialogs answer through callbacks, so the port shows the dialog and runs
 * a nested main loop until it answers (au_gtk_modal_wait). While one
 * waits, the network watches are off, the tick does nothing and input to
 * browser windows is ignored, so the model is never entered again from
 * inside its own call (docs/gtk.md, "Dialogs"). */
#include "gtk.h"

#include "browser/url.h"

#include <stdio.h>
#include <string.h>

/* ---- Waiting ---- */

void au_gtk_modal_begin(void)
{
    if (au_gtk.modal++ == 0)
        au_gtk_net_pause(true);
}

void au_gtk_modal_end(void)
{
    if (--au_gtk.modal == 0) {
        au_gtk_net_pause(false);
        au_gtk_kick();
    }
}

void au_gtk_modal_wait(const bool *done)
{
    au_gtk_modal_begin();
    while (!*done)
        g_main_context_iteration(NULL, TRUE);
    au_gtk_modal_end();
}

typedef struct Ask {
    bool done;
    int choice;
} Ask;

static void ask_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Ask *a = data;
    a->choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
    a->done = true;
}

bool au_gtk_ask(GtkWindow *parent, const char *message, const char *detail, const char *yes, const char *no)
{
    GtkAlertDialog *d = gtk_alert_dialog_new("%s", message);
    if (detail)
        gtk_alert_dialog_set_detail(d, detail);
    const char *buttons[] = {no, yes, NULL};
    gtk_alert_dialog_set_buttons(d, buttons);
    gtk_alert_dialog_set_cancel_button(d, 0);
    gtk_alert_dialog_set_default_button(d, 1);
    gtk_alert_dialog_set_modal(d, TRUE);
    Ask a = {false, -1};
    gtk_alert_dialog_choose(d, parent, NULL, ask_done, &a);
    au_gtk_modal_wait(&a.done);
    g_object_unref(d);
    return a.choice == 1;
}

void au_gtk_notice(GtkWindow *parent, const char *message, const char *detail)
{
    GtkAlertDialog *d = gtk_alert_dialog_new("%s", message);
    if (detail)
        gtk_alert_dialog_set_detail(d, detail);
    gtk_alert_dialog_set_modal(d, FALSE);
    gtk_alert_dialog_show(d, parent);
    g_object_unref(d);
}

void au_gtk_copy_text(const char *utf8, size_t len)
{
    GdkDisplay *d = gdk_display_get_default();
    if (!d)
        return;
    char *s = g_strndup(utf8, len);
    gdk_clipboard_set_text(gdk_display_get_clipboard(d), s);
    g_free(s);
}

static GtkWindow *parent_of(AuBrowserWindow *bw)
{
    AuGtkWin *w = bw ? au_gtk_win_of(bw) : au_gtk_front();
    return w ? w->window : NULL;
}

/* ---- AuBrowserHost ---- */

static uint32_t h_time_ms(void *ctx)
{
    (void)ctx;
    return au_posix_ms();
}

static AuTime h_now(void *ctx)
{
    (void)ctx;
    return (AuTime)au_posix_wall_time();
}

static bool h_read_file_url(void *ctx, const char *url, char **data, size_t *len)
{
    (void)ctx;
    char *path = au_url_to_path(url, '/');
    if (!path)
        return false;
    bool ok = au_platform_read_file(path, data, len);
    au_free(path);
    return ok;
}

static void h_invalidate(void *ctx, AuBrowserWindow *bw, AuIRect r)
{
    AuGtkWin *w = au_gtk_win_of(bw);
    (void)ctx;
    if (w) {
        au_present_invalidate(&w->present, r);
        au_gtk_kick();
    }
}

static void h_scroll_area(void *ctx, AuBrowserWindow *bw, AuIRect area, int dx, int dy)
{
    AuGtkWin *w = au_gtk_win_of(bw);
    (void)ctx;
    if (w)
        au_present_scroll(&w->present, area, dx, dy);
}

static void h_changed(void *ctx, AuBrowserWindow *bw, unsigned what)
{
    AuGtkWin *w = au_gtk_win_of(bw);
    (void)ctx;
    if (w)
        au_gtk_win_changed(w, what);
}

static void h_close_window(void *ctx, AuBrowserWindow *bw)
{
    AuGtkWin *w = au_gtk_win_of(bw);
    (void)ctx;
    /* Freed at the next flush, outside the model's call. */
    if (w) {
        w->closing = true;
        au_gtk_kick();
    }
}

static void launch_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *err = NULL;
    (void)data;
    if (!gtk_uri_launcher_launch_finish(GTK_URI_LAUNCHER(src), res, &err)) {
        fprintf(stderr, "aura: cannot open %s: %s\n", gtk_uri_launcher_get_uri(GTK_URI_LAUNCHER(src)),
                err ? err->message : "no handler");
        g_clear_error(&err);
    }
    g_object_unref(src);
}

static bool h_open_external(void *ctx, const char *url)
{
    (void)ctx;
    GtkUriLauncher *l = gtk_uri_launcher_new(url);
    gtk_uri_launcher_launch(l, parent_of(NULL), NULL, launch_done, NULL);
    return true;
}

static bool h_confirm_download(void *ctx, const char *url, const char *name, const char *mime)
{
    (void)ctx;
    (void)url;
    if (!au_gtk.browser->prefs.warn_app_downloads || !au_gtk_download_is_app(name, mime))
        return true;
    char msg[512];
    snprintf(msg, sizeof msg, "\"%s\" is a program.", name);
    return au_gtk_ask(parent_of(NULL), msg, "Programs from the web can harm your computer. Download it anyway?",
                      "Download", "Cancel");
}

/* The pop-up menu of a select: a GtkPopoverMenu at the control. The
 * enabled options share one radio action, so the selected one shows a
 * mark; disabled options and group labels use a disabled action. */
typedef struct Popup {
    bool done, closed;
    int choice;
} Popup;

static void popup_choose(GSimpleAction *a, GVariant *param, gpointer data)
{
    Popup *p = data;
    (void)a;
    p->choice = g_variant_get_int32(param);
    p->done = true;
}

static gboolean popup_finish(gpointer data)
{
    Popup *p = data;
    p->done = true;
    return G_SOURCE_REMOVE;
}

static void popup_closed(GtkPopover *po, gpointer data)
{
    (void)po;
    /* The item's action may come after closed: decide at the next idle. */
    g_idle_add(popup_finish, data);
}

static int h_popup(void *ctx, AuBrowserWindow *bw, const AuPopupItem *items, int n, int selected, AuIRect at)
{
    (void)ctx;
    AuGtkWin *w = au_gtk_win_of(bw);
    if (!w || n <= 0 || au_gtk.modal)
        return -1;
    GMenu *menu = g_menu_new();
    for (int i = 0; i < n; i++) {
        char *label = g_strdup_printf("%s%s", items[i].indent ? "    " : "", items[i].label ? items[i].label : "");
        GMenuItem *it = g_menu_item_new(label, NULL);
        if (items[i].disabled || items[i].group)
            g_menu_item_set_action_and_target_value(it, "popup.none", NULL);
        else
            g_menu_item_set_action_and_target_value(it, "popup.choose", g_variant_new_int32(i));
        g_menu_append_item(menu, it);
        g_object_unref(it);
        g_free(label);
    }
    Popup p = {false, false, -1};
    GSimpleActionGroup *group = g_simple_action_group_new();
    GSimpleAction *choose =
        g_simple_action_new_stateful("choose", G_VARIANT_TYPE_INT32, g_variant_new_int32(selected));
    g_signal_connect(choose, "activate", G_CALLBACK(popup_choose), &p);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(choose));
    GSimpleAction *none = g_simple_action_new("none", NULL);
    g_simple_action_set_enabled(none, FALSE);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(none));
    gtk_widget_insert_action_group(w->area, "popup", G_ACTION_GROUP(group));

    GtkWidget *po = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_widget_set_parent(po, w->area);
    GdkRectangle r = {at.x0, at.y0, at.x1 - at.x0, at.y1 - at.y0};
    gtk_popover_set_pointing_to(GTK_POPOVER(po), &r);
    gtk_popover_set_position(GTK_POPOVER(po), GTK_POS_BOTTOM);
    gtk_popover_set_has_arrow(GTK_POPOVER(po), FALSE);
    gtk_widget_set_halign(po, GTK_ALIGN_START);
    gulong sig = g_signal_connect(po, "closed", G_CALLBACK(popup_closed), &p);
    gtk_popover_popup(GTK_POPOVER(po));
    au_gtk_modal_wait(&p.done);
    g_signal_handler_disconnect(po, sig);
    /* A pending popup_finish idle may still point at p: drop it. */
    while (g_idle_remove_by_data(&p))
        ;
    gtk_widget_unparent(po);
    gtk_widget_insert_action_group(w->area, "popup", NULL);
    g_object_unref(choose);
    g_object_unref(none);
    g_object_unref(group);
    g_object_unref(menu);
    return p.choice;
}

typedef struct Choose {
    bool done;
    GListModel *files;
} Choose;

static void choose_one_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Choose *c = data;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f) {
        GListStore *s = g_list_store_new(G_TYPE_FILE);
        g_list_store_append(s, f);
        g_object_unref(f);
        c->files = G_LIST_MODEL(s);
    }
    c->done = true;
}

static void choose_many_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Choose *c = data;
    c->files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    c->done = true;
}

static bool h_choose_file(void *ctx, bool multiple, AuControlFile **files, uint32_t *n)
{
    (void)ctx;
    if (au_gtk.modal)
        return false;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_modal(d, TRUE);
    Choose c = {false, NULL};
    if (multiple)
        gtk_file_dialog_open_multiple(d, parent_of(NULL), NULL, choose_many_done, &c);
    else
        gtk_file_dialog_open(d, parent_of(NULL), NULL, choose_one_done, &c);
    au_gtk_modal_wait(&c.done);
    g_object_unref(d);
    if (!c.files)
        return false;
    guint count = g_list_model_get_n_items(c.files);
    AuControlFile *out = count ? au_calloc(sizeof *out * count) : NULL;
    uint32_t got = 0;
    for (guint i = 0; i < count; i++) {
        GFile *f = g_list_model_get_item(c.files, i);
        char *path = g_file_get_path(f);
        char *data;
        size_t len;
        if (path && au_platform_read_file(path, &data, &len)) {
            char *base = g_path_get_basename(path);
            char *type = g_content_type_guess(path, (const guchar *)data, len < 4096 ? len : 4096, NULL);
            char *mime = type ? g_content_type_get_mime_type(type) : NULL;
            const char *m = mime ? mime : "application/octet-stream";
            out[got].name = au_strndup(base, strlen(base));
            out[got].type = au_strndup(m, strlen(m));
            out[got].data = data;
            out[got].len = len;
            got++;
            g_free(base);
            g_free(type);
            g_free(mime);
        }
        g_free(path);
        g_object_unref(f);
    }
    g_object_unref(c.files);
    if (!got) {
        au_free(out);
        return false;
    }
    *files = out;
    *n = got;
    return true;
}

static bool h_confirm(void *ctx, AuBrowserWindow *bw, const char *message)
{
    (void)ctx;
    if (au_gtk.modal)
        return false;
    return au_gtk_ask(parent_of(bw), message, NULL, "OK", "Cancel");
}

static bool h_random(void *ctx, void *buf, size_t len)
{
    (void)ctx;
    return au_posix_entropy(buf, len);
}

static uint32_t h_memory_free_kb(void *ctx)
{
    (void)ctx;
    return au_gtk_memory_free_kb();
}

void au_gtk_browser_host(AuBrowserHost *h)
{
    memset(h, 0, sizeof *h);
    h->time_ms = h_time_ms;
    h->now = h_now;
    h->read_file_url = h_read_file_url;
    h->invalidate = h_invalidate;
    h->changed = h_changed;
    h->close_window = h_close_window;
    h->open_external = h_open_external;
    h->confirm_download = h_confirm_download;
    h->popup = h_popup;
    h->choose_file = h_choose_file;
    h->confirm = h_confirm;
    h->scroll_area = h_scroll_area;
    h->random = h_random;
    h->coro = au_posix_coro_ops();
    h->memory_free_kb = h_memory_free_kb;
}
