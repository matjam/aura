/* Helpers shared by the secondary windows: the host, the nested-loop
 * dialogs, the clipboard and the shared CSS (winutil_gtk.h). */
#include "winutil_gtk.h"

#include <string.h>
#include <time.h>

AuGtkWindowsHost au_gtk_host;

static int g_dialogs; /* nested dialogs running */

bool au_gtk_dialog_running(void) { return g_dialogs > 0; }

AuTime au_gtk_now(void) { return (AuTime)time(NULL); }

GtkWindow *au_gtk_front_parent(void)
{
    return au_gtk_host.front_gtk_window ? au_gtk_host.front_gtk_window(au_gtk_host.ctx) : NULL;
}

void au_gtk_windows_changed(void)
{
    if (au_gtk_host.windows_changed)
        au_gtk_host.windows_changed(au_gtk_host.ctx);
}

void au_gtk_window_css(void)
{
    static bool done;
    GdkDisplay *d = gdk_display_get_default();
    if (done || !d)
        return;
    done = true;
    GtkCssProvider *p = gtk_css_provider_new();
    /* The developer windows' controls sit on the model's rectangles,
     * which were drawn for Mac OS 9's small controls. */
    gtk_css_provider_load_from_string(p, ".aura-dev button { min-height: 0; padding: 1px 8px; font-size: 9pt; }\n"
                                         ".aura-dev checkbutton { min-height: 0; padding: 0; font-size: 9pt; }\n"
                                         ".aura-dev checkbutton check, .aura-dev checkbutton radio {"
                                         " min-width: 12px; min-height: 12px; margin: 0 4px 0 0; }\n"
                                         ".aura-dev scrollbar { background: #f4f4f4; }\n"
                                         ".aura-prefs-note { font-size: 9pt; }\n"
                                         ".aura-prefs-status { font-size: 9pt; opacity: 0.75; }\n");
    gtk_style_context_add_provider_for_display(d, GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

/* ---- Nested dialogs ---- */

typedef struct Dialog {
    GMainLoop *loop;
    GtkWindow *win;
    bool ok;
} Dialog;

static void dialog_finish(Dialog *d, bool ok)
{
    d->ok = ok;
    if (g_main_loop_is_running(d->loop))
        g_main_loop_quit(d->loop);
}

static gboolean dialog_close_request(GtkWindow *w, gpointer data)
{
    (void)w;
    dialog_finish(data, false);
    return TRUE; /* destroyed by the caller of the loop */
}

static void dialog_ok(GtkWidget *b, gpointer data)
{
    (void)b;
    dialog_finish(data, true);
}

static void dialog_cancel(GtkWidget *b, gpointer data)
{
    (void)b;
    dialog_finish(data, false);
}

static gboolean dialog_escape(GtkWidget *w, GVariant *args, gpointer data)
{
    (void)w;
    (void)args;
    dialog_finish(data, false);
    return TRUE;
}

/* A modal window with content above Cancel and OK. */
static GtkWidget *dialog_new(Dialog *d, GtkWindow *parent, const char *title, GtkWidget *content,
                             GtkWidget **ok_out)
{
    d->loop = g_main_loop_new(NULL, FALSE);
    d->ok = false;
    GtkWidget *w = gtk_window_new();
    d->win = GTK_WINDOW(w);
    gtk_window_set_title(d->win, title);
    gtk_window_set_modal(d->win, TRUE);
    gtk_window_set_resizable(d->win, FALSE);
    if (parent)
        gtk_window_set_transient_for(d->win, parent);
    g_signal_connect(w, "close-request", G_CALLBACK(dialog_close_request), d);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_box_append(GTK_BOX(box), content);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_mnemonic("_Cancel");
    GtkWidget *ok = gtk_button_new_with_mnemonic("_OK");
    gtk_widget_add_css_class(ok, "suggested-action");
    g_signal_connect(cancel, "clicked", G_CALLBACK(dialog_cancel), d);
    g_signal_connect(ok, "clicked", G_CALLBACK(dialog_ok), d);
    gtk_box_append(GTK_BOX(row), cancel);
    gtk_box_append(GTK_BOX(row), ok);
    gtk_box_append(GTK_BOX(box), row);
    gtk_window_set_child(d->win, box);
    gtk_window_set_default_widget(d->win, ok);

    GtkEventController *sc = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(
        GTK_SHORTCUT_CONTROLLER(sc),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0), gtk_callback_action_new(dialog_escape, d, NULL)));
    gtk_widget_add_controller(w, sc);
    *ok_out = ok;
    return w;
}

static void entry_activate(GtkEntry *e, gpointer data)
{
    (void)e;
    dialog_finish(data, true);
}

char *au_gtk_ask_text(GtkWindow *parent, const char *title, const char *prompt, const char *initial)
{
    Dialog d;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *label = gtk_label_new(prompt ? prompt : "");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 50);
    GtkWidget *entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), initial ? initial : "");
    gtk_editable_set_width_chars(GTK_EDITABLE(entry), 40);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), entry);
    GtkWidget *ok;
    dialog_new(&d, parent, title ? title : "Aura", box, &ok);
    g_signal_connect(entry, "activate", G_CALLBACK(entry_activate), &d);
    gtk_widget_grab_focus(entry);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
    char *text = NULL;
    /* The entry is read before the window is destroyed. */
    g_dialogs++;
    gtk_window_present(d.win);
    g_main_loop_run(d.loop);
    g_dialogs--;
    if (d.ok) {
        const char *t = gtk_editable_get_text(GTK_EDITABLE(entry));
        text = au_strndup(t, strlen(t));
    }
    gtk_window_destroy(d.win);
    g_main_loop_unref(d.loop);
    return text;
}

int au_gtk_choose(GtkWindow *parent, const char *title, const char *prompt, const char *const *items, int n,
                  int current)
{
    Dialog d;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    if (prompt) {
        GtkWidget *label = gtk_label_new(prompt);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_box_append(GTK_BOX(box), label);
    }
    GtkWidget **radios = g_new0(GtkWidget *, n > 0 ? n : 1);
    for (int i = 0; i < n; i++) {
        radios[i] = gtk_check_button_new_with_label(items[i]);
        if (i > 0)
            gtk_check_button_set_group(GTK_CHECK_BUTTON(radios[i]), GTK_CHECK_BUTTON(radios[0]));
        if (i == current)
            gtk_check_button_set_active(GTK_CHECK_BUTTON(radios[i]), TRUE);
        gtk_box_append(GTK_BOX(box), radios[i]);
    }
    GtkWidget *ok;
    dialog_new(&d, parent, title ? title : "Aura", box, &ok);
    gtk_widget_grab_focus(ok);
    int chosen = -1;
    g_dialogs++;
    gtk_window_present(d.win);
    g_main_loop_run(d.loop);
    g_dialogs--;
    if (d.ok)
        for (int i = 0; i < n; i++)
            if (gtk_check_button_get_active(GTK_CHECK_BUTTON(radios[i])))
                chosen = i;
    gtk_window_destroy(d.win);
    g_main_loop_unref(d.loop);
    g_free(radios);
    return chosen;
}

static void text_close(GtkWidget *b, gpointer win)
{
    (void)b;
    gtk_window_destroy(GTK_WINDOW(win));
}

void au_gtk_show_text(GtkWindow *parent, const char *title, const char *text)
{
    GtkWidget *w = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(w), title ? title : "Details");
    if (parent)
        gtk_window_set_transient_for(GTK_WINDOW(w), parent);
    gtk_window_set_default_size(GTK_WINDOW(w), 460, 300);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    /* "Label: value" lines in two columns; other lines span both. */
    int row = 0;
    const char *p = text ? text : "";
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char *line = g_strndup(p, len);
        char *colon = strstr(line, ": ");
        if (colon) {
            *colon = 0;
            GtkWidget *k = gtk_label_new(line);
            gtk_label_set_xalign(GTK_LABEL(k), 1);
            gtk_widget_set_valign(k, GTK_ALIGN_START);
            gtk_widget_add_css_class(k, "dim-label");
            GtkWidget *v = gtk_label_new(colon + 2);
            gtk_label_set_xalign(GTK_LABEL(v), 0);
            gtk_label_set_wrap(GTK_LABEL(v), TRUE);
            gtk_label_set_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_selectable(GTK_LABEL(v), TRUE);
            gtk_widget_set_hexpand(v, TRUE);
            gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
        } else {
            GtkWidget *v = gtk_label_new(line);
            gtk_label_set_xalign(GTK_LABEL(v), 0);
            gtk_label_set_wrap(GTK_LABEL(v), TRUE);
            gtk_label_set_selectable(GTK_LABEL(v), TRUE);
            gtk_grid_attach(GTK_GRID(grid), v, 0, row, 2, 1);
        }
        g_free(line);
        row++;
        p += len;
        if (*p == '\n')
            p++;
    }
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), grid);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_box_append(GTK_BOX(box), sw);
    GtkWidget *close = gtk_button_new_with_mnemonic("_Close");
    gtk_widget_set_halign(close, GTK_ALIGN_END);
    g_signal_connect(close, "clicked", G_CALLBACK(text_close), w);
    gtk_box_append(GTK_BOX(box), close);
    gtk_window_set_child(GTK_WINDOW(w), box);
    gtk_window_present(GTK_WINDOW(w));
}

void au_gtk_alert(GtkWindow *parent, const char *message, const char *detail)
{
    GtkAlertDialog *a = gtk_alert_dialog_new("%s", message);
    if (detail)
        gtk_alert_dialog_set_detail(a, detail);
    gtk_alert_dialog_show(a, parent);
    g_object_unref(a);
}

void au_gtk_copy(GtkWidget *w, const char *text, size_t len)
{
    char *s = g_strndup(text, len);
    gdk_clipboard_set_text(gtk_widget_get_clipboard(w), s);
    g_free(s);
}

/* ---- Entry points that span the files ---- */

void au_gtk_windows_init(const AuGtkWindowsHost *host) { au_gtk_host = *host; }

void au_gtk_windows_shutdown(void)
{
    au_gtk_prefs_close();
    au_gtk_lists_close();
    au_gtk_devwins_close();
    au_gtk_about_close();
}

bool au_gtk_window_kind(AuWindowKind kind)
{
    switch (kind) {
    case AU_WINDOW_ABOUT: au_gtk_about_show(); return true;
    case AU_WINDOW_PREFERENCES: au_gtk_prefs_show(); return true;
    case AU_WINDOW_HISTORY: au_gtk_list_show(AU_LIST_HISTORY); return true;
    case AU_WINDOW_BOOKMARKS: au_gtk_list_show(AU_LIST_BOOKMARKS); return true;
    case AU_WINDOW_DOWNLOADS: au_gtk_list_show(AU_LIST_DOWNLOADS); return true;
    case AU_WINDOW_COOKIES: au_gtk_list_show(AU_LIST_COOKIES); return true;
    case AU_WINDOW_PAGE_SOURCE: au_gtk_devwin_show(AU_DEVWIN_SOURCE); return true;
    case AU_WINDOW_CONSOLE: au_gtk_devwin_show(AU_DEVWIN_CONSOLE); return true;
    case AU_WINDOW_NETWORK: au_gtk_devwin_show(AU_DEVWIN_NETWORK); return true;
    case AU_WINDOW_PERFORMANCE: au_gtk_devwin_show(AU_DEVWIN_PERF); return true;
    case AU_WINDOW_SYSTEM_STATUS: au_gtk_devwin_show(AU_DEVWIN_STATUS); return true;
    case AU_WINDOW_REPORT: au_gtk_devwin_show(AU_DEVWIN_REPORT); return true;
    case AU_WINDOW_TIMING_TEST: au_gtk_devwin_run_timing(); return true;
    case AU_WINDOW_PAINT_BENCHMARK: au_gtk_devwin_run_benchmark(); return true;
    default: return false;
    }
}
