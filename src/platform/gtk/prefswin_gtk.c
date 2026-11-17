/* The Preferences window.
 *
 * Every item, its kind, caption, choices, enabling rule, status line and
 * what a change does is the edit session in src/browser/prefs_ui.c. This
 * file builds the window from the descriptions alone and holds only what
 * is about GTK:
 *   - one GtkStack page per section, chosen in a GtkStackSidebar; on a
 *     page a two-column grid of caption and control, one control per
 *     item by its kind (check button, radio pair, number entry with its
 *     unit, text entry, drop-down, folder entry with Choose, button, note,
 *     the blocking lists' rows);
 *   - turning a change into the session's setter: check boxes, radio
 *     buttons and drop-downs at once (live items applied to the browser),
 *     text and number fields when OK is clicked, as on Mac OS 9;
 *   - OK (commit), Cancel and closing the window (cancel), Restore
 *     Defaults (the visible section), and the buttons' actions;
 *   - a timer that redraws the blocking lists' status lines while they
 *     download. */
#include "winutil_gtk.h"

#include <stdio.h>
#include <string.h>

typedef struct PrefsWin {
    AuPrefsEdit *edit;
    GtkWindow *win;
    GtkWidget *stack;
    GtkWidget *ctl[AU_PREF_COUNT];    /* the control; a radio item's first button */
    GtkWidget *ctl2[AU_PREF_COUNT];   /* a radio item's second button; a folder's Choose button */
    GtkWidget *status[AU_PREF_COUNT]; /* the status line or note text */
    GtkWidget *block_box;             /* the blocking lists' rows */
    GtkWidget **block_check, **block_status;
    size_t nblock;
    uint32_t block_gen;
    bool loading; /* controls are being set from the session */
    guint timer;
} PrefsWin;

static PrefsWin *g_prefs;

static const AuPrefDesc *desc(AuPrefId id) { return au_pref_desc(id); }

/* ---- Values ---- */

static void load_item(PrefsWin *p, AuPrefId id)
{
    GtkWidget *c = p->ctl[id];
    if (!c)
        return;
    p->loading = true;
    switch (desc(id)->kind) {
    case AU_PREF_CHECK: gtk_check_button_set_active(GTK_CHECK_BUTTON(c), au_prefs_edit_get_bool(p->edit, id)); break;
    case AU_PREF_RADIO: {
        int v = au_prefs_edit_get_choice(p->edit, id);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(v == 1 ? p->ctl2[id] : c), TRUE);
        break;
    }
    case AU_PREF_NUMBER:
    case AU_PREF_TEXT:
    case AU_PREF_FOLDER: {
        AuBuf b;
        au_buf_init(&b);
        au_prefs_edit_get_text(p->edit, id, &b);
        gtk_editable_set_text(GTK_EDITABLE(c), au_buf_cstr(&b));
        au_buf_free(&b);
        break;
    }
    case AU_PREF_CHOICE:
    case AU_PREF_FONT: {
        int v = au_prefs_edit_get_choice(p->edit, id);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(c), v >= 0 ? (guint)v : GTK_INVALID_LIST_POSITION);
        break;
    }
    default: break;
    }
    p->loading = false;
}

static void load_blocking(PrefsWin *p)
{
    AuPrefBlockRow row;
    p->loading = true;
    for (size_t i = 0; i < p->nblock; i++) {
        if (!au_prefs_edit_block_list(p->edit, i, &row))
            continue;
        gtk_check_button_set_active(GTK_CHECK_BUTTON(p->block_check[i]), row.on);
        gtk_label_set_text(GTK_LABEL(p->block_status[i]), row.status);
    }
    p->block_gen = au_prefs_edit_block_generation(p->edit);
    p->loading = false;
}

/* Status lines and enabled states, after a change. */
static void refresh_states(PrefsWin *p)
{
    AuBuf b;
    au_buf_init(&b);
    for (int i = 0; i < AU_PREF_COUNT; i++) {
        AuPrefId id = (AuPrefId)i;
        if (p->status[id]) {
            au_buf_clear(&b);
            au_prefs_edit_text(p->edit, id, &b);
            if (strcmp(au_buf_cstr(&b), gtk_label_get_text(GTK_LABEL(p->status[id]))))
                gtk_label_set_text(GTK_LABEL(p->status[id]), au_buf_cstr(&b));
            gtk_widget_set_visible(p->status[id], b.len > 0);
        }
        if (!p->ctl[id])
            continue;
        bool on = au_prefs_edit_enabled(p->edit, id);
        gtk_widget_set_sensitive(p->ctl[id], on);
        if (p->ctl2[id])
            gtk_widget_set_sensitive(p->ctl2[id], on);
    }
    au_buf_free(&b);
}

static void load_all(PrefsWin *p)
{
    for (int i = 0; i < AU_PREF_COUNT; i++)
        load_item(p, (AuPrefId)i);
    load_blocking(p);
    refresh_states(p);
}

/* OK: the fields' text into the session (the other kinds are set as they
 * change). */
static void store(PrefsWin *p)
{
    for (int i = 0; i < AU_PREF_COUNT; i++) {
        AuPrefId id = (AuPrefId)i;
        if (!p->ctl[id])
            continue;
        AuPrefKind k = desc(id)->kind;
        if (k == AU_PREF_NUMBER || k == AU_PREF_TEXT || k == AU_PREF_FOLDER)
            au_prefs_edit_set_text(p->edit, id, gtk_editable_get_text(GTK_EDITABLE(p->ctl[id])));
    }
}

static uint32_t memory_free_kb(void)
{
    return au_gtk_host.memory_free_kb ? au_gtk_host.memory_free_kb(au_gtk_host.ctx) : 0;
}

static AuDefaultBrowser default_browser(void)
{
    if (!au_gtk_host.default_browser || !au_gtk_host.make_default_browser)
        return AU_DEFAULT_BROWSER_UNAVAILABLE;
    return au_gtk_host.default_browser(au_gtk_host.ctx);
}

static void blocking_changed(void)
{
    if (au_gtk_host.blocking_changed)
        au_gtk_host.blocking_changed(au_gtk_host.ctx);
}

/* A live item changed in the session: apply it to the browser now. */
static void apply_live(PrefsWin *p)
{
    unsigned bits = au_prefs_edit_apply_live(p->edit, au_gtk_host.browser);
    if (bits & AU_PREF_CHANGED_BLOCKING)
        blocking_changed();
    if (bits && au_gtk_host.prefs_changed)
        au_gtk_host.prefs_changed(au_gtk_host.ctx, bits, false);
}

/* After a check box, radio button or drop-down changed id. */
static void changed(PrefsWin *p, AuPrefId id)
{
    if (desc(id)->flags & AU_PREF_LIVE)
        apply_live(p);
    /* Automatic script memory follows the memory free now. */
    au_prefs_edit_set_memory_free(p->edit, memory_free_kb());
    refresh_states(p);
}

/* ---- Actions ---- */

static void run_action(PrefsWin *p, AuPrefId id)
{
    AuBrowserWindow *front = au_gtk_host.front_window ? au_gtk_host.front_window(au_gtk_host.ctx) : NULL;
    AuPrefId what = AU_PREF_NONE;
    unsigned todo = au_prefs_edit_action(p->edit, id, au_gtk_host.browser, front, &what);
    if (todo & AU_PREF_DO_RELOAD)
        load_item(p, what);
    if (todo & AU_PREF_DO_MAKE_DEFAULT) {
        if (!au_gtk_host.make_default_browser || !au_gtk_host.make_default_browser(au_gtk_host.ctx))
            gtk_widget_error_bell(GTK_WIDGET(p->win));
        au_prefs_edit_set_default_browser(p->edit, default_browser());
    }
    if (todo & AU_PREF_DO_CLEAR_COOKIES) {
        if (au_gtk_host.aura_app)
            au_app_clear_cookies(au_gtk_host.aura_app);
        au_gtk_list_touch(AU_LIST_COOKIES);
    }
    if ((todo & AU_PREF_DO_CLEAR_CACHE) && au_gtk_host.aura_app)
        au_app_clear_cache(au_gtk_host.aura_app);
    if (todo & AU_PREF_DO_SHOW_COOKIES)
        au_gtk_list_show(AU_LIST_COOKIES);
    if (todo & AU_PREF_DO_REFRESH_LISTS)
        load_blocking(p);
    refresh_states(p);
}

/* ---- Signals ---- */

#define ITEM(w) ((AuPrefId)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "aura-pref")))

static void check_toggled(GtkCheckButton *b, gpointer data)
{
    PrefsWin *p = data;
    if (p->loading)
        return;
    AuPrefId id = ITEM(b);
    au_prefs_edit_set_bool(p->edit, id, gtk_check_button_get_active(b));
    changed(p, id);
}

static void radio_toggled(GtkCheckButton *b, gpointer data)
{
    PrefsWin *p = data;
    if (p->loading || !gtk_check_button_get_active(b))
        return;
    AuPrefId id = ITEM(b);
    au_prefs_edit_set_choice(p->edit, id, GTK_WIDGET(b) == p->ctl2[id] ? 1 : 0);
    changed(p, id);
}

static void drop_changed(GtkDropDown *d, GParamSpec *ps, gpointer data)
{
    PrefsWin *p = data;
    if (p->loading)
        return;
    AuPrefId id = ITEM(d);
    guint v = gtk_drop_down_get_selected(d);
    if (v == GTK_INVALID_LIST_POSITION)
        return;
    if (!au_prefs_edit_set_choice(p->edit, id, (int)v))
        load_item(p, id);
    changed(p, id);
}

static void action_clicked(GtkButton *b, gpointer data) { run_action(data, ITEM(b)); }

static void block_toggled(GtkCheckButton *b, gpointer data)
{
    PrefsWin *p = data;
    if (p->loading)
        return;
    size_t i = (size_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "aura-row"));
    au_prefs_edit_set_block_list(p->edit, i, gtk_check_button_get_active(b));
    apply_live(p);
    load_blocking(p);
    refresh_states(p);
}

static void folder_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
    GtkWidget *entry = data;
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f) {
        char *path = g_file_get_path(f);
        if (path && g_prefs && g_prefs->ctl[ITEM(entry)] == entry)
            gtk_editable_set_text(GTK_EDITABLE(entry), path);
        g_free(path);
        g_object_unref(f);
    }
    g_object_unref(entry);
}

static void choose_folder(GtkButton *b, gpointer data)
{
    PrefsWin *p = data;
    GtkWidget *entry = p->ctl[ITEM(b)];
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Choose a Download Folder");
    const char *cur = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (cur[0] == '/') {
        GFile *f = g_file_new_for_path(cur);
        gtk_file_dialog_set_initial_folder(d, f);
        g_object_unref(f);
    }
    gtk_file_dialog_select_folder(d, p->win, NULL, folder_chosen, g_object_ref(entry));
    g_object_unref(d);
}

/* ---- Building ---- */

static GtkWidget *small_label(const char *text, const char *css)
{
    GtkWidget *l = gtk_label_new(text ? text : "");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
    gtk_widget_add_css_class(l, css);
    return l;
}

static void tag(GtkWidget *w, AuPrefId id) { g_object_set_data(G_OBJECT(w), "aura-pref", GINT_TO_POINTER(id)); }

static GtkWidget *build_blocking(PrefsWin *p)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    p->nblock = au_prefs_edit_block_list_count(p->edit);
    p->block_check = g_new0(GtkWidget *, p->nblock + 1);
    p->block_status = g_new0(GtkWidget *, p->nblock + 1);
    AuPrefBlockRow row;
    for (size_t i = 0; i < p->nblock; i++) {
        if (!au_prefs_edit_block_list(p->edit, i, &row))
            memset(&row, 0, sizeof row);
        GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        p->block_check[i] = gtk_check_button_new_with_label(row.title ? row.title : "");
        g_object_set_data(G_OBJECT(p->block_check[i]), "aura-row", GINT_TO_POINTER((int)i));
        g_signal_connect(p->block_check[i], "toggled", G_CALLBACK(block_toggled), p);
        p->block_status[i] = small_label(row.status, "aura-prefs-status");
        gtk_widget_set_hexpand(p->block_status[i], TRUE);
        gtk_label_set_xalign(GTK_LABEL(p->block_status[i]), 1);
        gtk_box_append(GTK_BOX(line), p->block_check[i]);
        gtk_box_append(GTK_BOX(line), p->block_status[i]);
        gtk_box_append(GTK_BOX(box), line);
    }
    if (!p->nblock)
        gtk_box_append(GTK_BOX(box), small_label("The blocking lists are not available yet.", "aura-prefs-status"));
    p->block_box = box;
    return box;
}

static GtkStringList *choices_of(PrefsWin *p, AuPrefId id)
{
    GtkStringList *l = gtk_string_list_new(NULL);
    size_t n = au_prefs_edit_choice_count(p->edit, id);
    for (size_t i = 0; i < n; i++)
        gtk_string_list_append(l, au_prefs_edit_choice(p->edit, id, i));
    if (!n && desc(id)->empty)
        gtk_string_list_append(l, desc(id)->empty);
    return l;
}

/* One item's row(s) of the grid; *row moves past them. */
static void build_item(PrefsWin *p, GtkGrid *g, AuPrefId id, int *row)
{
    const AuPrefDesc *d = desc(id);
    GtkWidget *control = NULL; /* what goes in the second column */
    switch (d->kind) {
    case AU_PREF_CHECK:
        control = p->ctl[id] = gtk_check_button_new_with_label(d->title ? d->title : "");
        g_signal_connect(control, "toggled", G_CALLBACK(check_toggled), p);
        break;
    case AU_PREF_RADIO: {
        control = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        const char *a = d->choices && d->choices[0] ? d->choices[0] : "";
        const char *b = d->choices && a[0] && d->choices[1] ? d->choices[1] : "";
        p->ctl[id] = gtk_check_button_new_with_label(a);
        p->ctl2[id] = gtk_check_button_new_with_label(b);
        gtk_check_button_set_group(GTK_CHECK_BUTTON(p->ctl2[id]), GTK_CHECK_BUTTON(p->ctl[id]));
        tag(p->ctl2[id], id);
        g_signal_connect(p->ctl[id], "toggled", G_CALLBACK(radio_toggled), p);
        g_signal_connect(p->ctl2[id], "toggled", G_CALLBACK(radio_toggled), p);
        gtk_box_append(GTK_BOX(control), p->ctl[id]);
        gtk_box_append(GTK_BOX(control), p->ctl2[id]);
        break;
    }
    case AU_PREF_NUMBER:
        control = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        p->ctl[id] = gtk_entry_new();
        gtk_editable_set_width_chars(GTK_EDITABLE(p->ctl[id]), 7);
        gtk_entry_set_input_purpose(GTK_ENTRY(p->ctl[id]), GTK_INPUT_PURPOSE_DIGITS);
        gtk_box_append(GTK_BOX(control), p->ctl[id]);
        if (d->unit)
            gtk_box_append(GTK_BOX(control), gtk_label_new(d->unit));
        break;
    case AU_PREF_TEXT:
        control = p->ctl[id] = gtk_entry_new();
        gtk_widget_set_hexpand(control, TRUE);
        break;
    case AU_PREF_FOLDER:
        control = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        p->ctl[id] = gtk_entry_new();
        gtk_widget_set_hexpand(p->ctl[id], TRUE);
        p->ctl2[id] = gtk_button_new_with_label("Choose…");
        tag(p->ctl2[id], id);
        g_signal_connect(p->ctl2[id], "clicked", G_CALLBACK(choose_folder), p);
        gtk_box_append(GTK_BOX(control), p->ctl[id]);
        gtk_box_append(GTK_BOX(control), p->ctl2[id]);
        break;
    case AU_PREF_CHOICE:
    case AU_PREF_FONT:
        control = p->ctl[id] = gtk_drop_down_new(G_LIST_MODEL(choices_of(p, id)), NULL);
        gtk_widget_set_halign(control, GTK_ALIGN_START);
        if (d->kind == AU_PREF_FONT)
            gtk_drop_down_set_enable_search(GTK_DROP_DOWN(control), TRUE);
        g_signal_connect(control, "notify::selected", G_CALLBACK(drop_changed), p);
        break;
    case AU_PREF_ACTION:
        control = p->ctl[id] = gtk_button_new_with_label(d->title ? d->title : "");
        gtk_widget_set_halign(control, GTK_ALIGN_START);
        g_signal_connect(control, "clicked", G_CALLBACK(action_clicked), p);
        break;
    case AU_PREF_NOTE:
        control = p->status[id] = small_label(d->help, "aura-prefs-note");
        break;
    case AU_PREF_LIST: control = build_blocking(p); break;
    }
    if (!control)
        return;
    if (p->ctl[id])
        tag(p->ctl[id], id);
    if (d->caption) {
        GtkWidget *cap = gtk_label_new(d->caption);
        gtk_label_set_xalign(GTK_LABEL(cap), 1);
        gtk_widget_set_valign(cap, d->kind == AU_PREF_LIST ? GTK_ALIGN_START : GTK_ALIGN_CENTER);
        gtk_grid_attach(g, cap, 0, *row, 1, 1);
    }
    gtk_grid_attach(g, control, 1, *row, 1, 1);
    (*row)++;
    if ((d->flags & AU_PREF_STATUS) && d->kind != AU_PREF_NOTE) {
        p->status[id] = small_label("", "aura-prefs-status");
        gtk_grid_attach(g, p->status[id], 1, *row, 1, 1);
        (*row)++;
    }
}

static GtkWidget *build_section(PrefsWin *p, AuPrefSection s)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_start(grid, 20);
    gtk_widget_set_margin_end(grid, 20);
    gtk_widget_set_margin_top(grid, 16);
    gtk_widget_set_margin_bottom(grid, 16);
    int row = 0;
    for (int i = 0; i < AU_PREF_COUNT; i++) {
        const AuPrefDesc *d = desc((AuPrefId)i);
        if (d && d->section == s)
            build_item(p, GTK_GRID(grid), (AuPrefId)i, &row);
    }
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), grid);
    return sw;
}

/* ---- The window ---- */

static void close_prefs(PrefsWin *p, bool ok)
{
    if (ok) {
        store(p);
        au_prefs_edit_commit(p->edit, au_gtk_host.browser);
        if (au_gtk_host.prefs_changed)
            au_gtk_host.prefs_changed(au_gtk_host.ctx, 0, true);
    } else {
        unsigned bits = au_prefs_edit_cancel(p->edit, au_gtk_host.browser);
        if (bits & AU_PREF_CHANGED_BLOCKING)
            blocking_changed();
        if (bits && au_gtk_host.prefs_changed)
            au_gtk_host.prefs_changed(au_gtk_host.ctx, bits, false);
    }
    gtk_window_destroy(p->win);
}

static void ok_clicked(GtkButton *b, gpointer data) { close_prefs(data, true); }
static void cancel_clicked(GtkButton *b, gpointer data) { close_prefs(data, false); }

static void defaults_clicked(GtkButton *b, gpointer data)
{
    PrefsWin *p = data;
    const char *name = gtk_stack_get_visible_child_name(GTK_STACK(p->stack));
    for (int s = 0; s < AU_PREF_SECTION_COUNT && name; s++)
        if (!strcmp(name, au_pref_section_title((AuPrefSection)s))) {
            au_prefs_edit_reset_section(p->edit, (AuPrefSection)s);
            apply_live(p);
            load_all(p);
        }
}

static gboolean close_request(GtkWindow *w, gpointer data)
{
    close_prefs(data, false);
    return TRUE;
}

static gboolean escape(GtkWidget *w, GVariant *args, gpointer data)
{
    close_prefs(data, false);
    return TRUE;
}

static gboolean tick(gpointer data)
{
    PrefsWin *p = data;
    if (au_prefs_edit_block_generation(p->edit) != p->block_gen) {
        load_blocking(p);
        refresh_states(p);
    }
    return G_SOURCE_CONTINUE;
}

static void destroyed(GtkWidget *w, gpointer data)
{
    PrefsWin *p = data;
    g_source_remove(p->timer);
    au_prefs_edit_free(p->edit);
    g_free(p->block_check);
    g_free(p->block_status);
    g_free(p);
    g_prefs = NULL;
    au_gtk_windows_changed();
}

static AuPrefsEdit *new_session(void)
{
    AuPrefsHost h;
    memset(&h, 0, sizeof h);
    h.fonts = au_gtk_host.fonts;
    h.memory_free_kb = memory_free_kb();
    h.blocking = au_gtk_host.block_manager ? au_gtk_host.block_manager(au_gtk_host.ctx) : NULL;
    h.default_browser = default_browser();
    h.machine = au_gtk_host.machine ? au_gtk_host.machine : "this computer";
    h.settings_name = au_gtk_host.settings_name ? au_gtk_host.settings_name : "the system settings";
    char example[512];
    snprintf(example, sizeof example, "%s/Downloads", g_get_home_dir());
    h.folder_example = au_gtk_host.folder_example ? au_gtk_host.folder_example : example;
    const char *xdg = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    char *xdg_name = xdg ? g_path_get_basename(xdg) : NULL;
    h.system_downloads = au_gtk_host.system_downloads ? au_gtk_host.system_downloads : xdg_name;
    h.no_folder = au_gtk_host.no_folder ? au_gtk_host.no_folder : "your home folder";
    AuPrefsEdit *e = au_prefs_edit_new(&au_gtk_host.browser->prefs, &h);
    g_free(xdg_name);
    return e;
}

void au_gtk_prefs_show(void)
{
    if (g_prefs) {
        gtk_window_present(g_prefs->win);
        return;
    }
    if (!au_gtk_host.browser)
        return;
    au_gtk_window_css();
    PrefsWin *p = g_new0(PrefsWin, 1);
    p->edit = new_session();
    GtkWidget *w = gtk_window_new();
    p->win = GTK_WINDOW(w);
    gtk_window_set_title(p->win, "Preferences");
    gtk_window_set_default_size(p->win, 760, 560);
    if (au_gtk_host.app)
        gtk_window_set_application(p->win, au_gtk_host.app);

    p->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(p->stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_widget_set_hexpand(p->stack, TRUE);
    gtk_widget_set_vexpand(p->stack, TRUE);
    for (int s = 0; s < AU_PREF_SECTION_COUNT; s++) {
        const char *title = au_pref_section_title((AuPrefSection)s);
        gtk_stack_add_titled(GTK_STACK(p->stack), build_section(p, (AuPrefSection)s), title, title);
    }
    GtkWidget *side = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(side), GTK_STACK(p->stack));
    gtk_widget_set_size_request(side, 170, -1);

    GtkWidget *panes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(panes), side);
    gtk_box_append(GTK_BOX(panes), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(panes), p->stack);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(bar, 12);
    gtk_widget_set_margin_end(bar, 12);
    gtk_widget_set_margin_top(bar, 10);
    gtk_widget_set_margin_bottom(bar, 10);
    GtkWidget *defaults = gtk_button_new_with_mnemonic("Restore _Defaults");
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    GtkWidget *cancel = gtk_button_new_with_mnemonic("_Cancel");
    GtkWidget *ok = gtk_button_new_with_mnemonic("_OK");
    gtk_widget_add_css_class(ok, "suggested-action");
    g_signal_connect(defaults, "clicked", G_CALLBACK(defaults_clicked), p);
    g_signal_connect(cancel, "clicked", G_CALLBACK(cancel_clicked), p);
    g_signal_connect(ok, "clicked", G_CALLBACK(ok_clicked), p);
    gtk_box_append(GTK_BOX(bar), defaults);
    gtk_box_append(GTK_BOX(bar), spacer);
    gtk_box_append(GTK_BOX(bar), cancel);
    gtk_box_append(GTK_BOX(bar), ok);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), panes);
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box), bar);
    gtk_window_set_child(p->win, box);
    gtk_window_set_default_widget(p->win, ok);
    /* Return in a field is OK, as on Mac OS 9. */
    for (int i = 0; i < AU_PREF_COUNT; i++)
        if (p->ctl[i] && GTK_IS_ENTRY(p->ctl[i]))
            gtk_entry_set_activates_default(GTK_ENTRY(p->ctl[i]), TRUE);

    GtkEventController *sc = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(
        GTK_SHORTCUT_CONTROLLER(sc),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0), gtk_callback_action_new(escape, p, NULL)));
    gtk_widget_add_controller(w, sc);
    g_signal_connect(w, "close-request", G_CALLBACK(close_request), p);
    g_signal_connect(w, "destroy", G_CALLBACK(destroyed), p);

    g_prefs = p;
    load_all(p);
    p->timer = g_timeout_add(1000, tick, p);
    GtkWindow *parent = au_gtk_front_parent();
    if (parent)
        gtk_window_set_transient_for(p->win, parent);
    gtk_window_present(p->win);
    au_gtk_windows_changed();
}

bool au_gtk_prefs_is_open(void) { return g_prefs != NULL; }

void au_gtk_prefs_close(void)
{
    if (g_prefs)
        close_prefs(g_prefs, false);
}
