/* The menu bar, its actions and accelerators, built from the command
 * table (browser/commands.h); the AuCommandHost; contextual menus; and
 * the Edit commands (docs/gtk.md, "Menus").
 *
 * Every command of the bar is the application action "app.cmd-N" (N its
 * AuCommand value) with the table's shortcut as its accelerator, Ctrl
 * for AU_SHORTCUT_PRIMARY. The tick refreshes which actions are enabled
 * and checked from au_command_enabled and au_command_checked; a
 * disabled action's accelerator passes its key on to the focused
 * widget. The Go, Bookmarks and Window menus end with a section rebuilt
 * from the model's dynamic lists when they change. */
#include "gtk.h"

#include <string.h>

typedef struct DynList {
    AuMenuDynamic kind;
    GMenu *section; /* the menu's last section, refilled */
    AuMenuList list;
    const char *action; /* "hist", "bm" or "win": "app.<action>" with the entry's index */
} DynList;

static struct {
    GSimpleAction *cmd[AU_CMD_COUNT];
    DynList dyn[3];
    int ndyn;
    bool dirty;      /* the history or bookmarks changed */
    guint win_hash;  /* the Window list as last built */
    bool built;
} M;

/* ---- State and running ---- */

static GtkWindow *active_window(void)
{
    return au_gtk.app ? gtk_application_get_active_window(au_gtk.app) : NULL;
}

void au_gtk_command_state(AuCommandState *st)
{
    memset(st, 0, sizeof *st);
    st->browser = au_gtk.browser;
    AuGtkWin *w = au_gtk_front();
    st->window = w ? w->bw : NULL;
    GtkWindow *act = active_window();
    if (!act)
        st->front = w ? AU_FRONT_BROWSER : AU_FRONT_NONE;
    else if (w && act == w->window)
        st->front = AU_FRONT_BROWSER;
    else
        st->front = AU_FRONT_OTHER;
    GtkWidget *focus = act ? gtk_root_get_focus(GTK_ROOT(act)) : NULL;
    st->text_focus = focus && GTK_IS_TEXT(focus);
    st->tool_can_find_again = false;
    st->mods = au_gtk_current_mods(act ? GTK_WIDGET(act) : NULL);
}

/* The command a shortcut-bearing action runs: its own, or what its key
 * means now (Close Tab's key closes a window without tabs). */
static AuCommand resolve(const AuCommandState *st, AuCommand cmd)
{
    const AuCommandInfo *ci = au_command_info(cmd);
    if (au_command_enabled(st, cmd) || !ci || !ci->shortcut.key)
        return cmd;
    return au_command_for_key(st, ci->shortcut.key, ci->shortcut.mods);
}

void au_gtk_run(AuCommand cmd)
{
    if (!au_gtk.started || au_gtk.quitting || au_gtk.modal)
        return;
    AuCommandState st;
    au_gtk_command_state(&st);
    au_command_run(&st, au_gtk_command_host(), resolve(&st, cmd));
    au_gtk_kick();
}

static void on_cmd(GSimpleAction *a, GVariant *param, gpointer data)
{
    (void)a;
    (void)param;
    au_gtk_run((AuCommand)GPOINTER_TO_INT(data));
}

static void on_entry(GSimpleAction *a, GVariant *param, gpointer data)
{
    DynList *d = data;
    (void)a;
    int i = g_variant_get_int32(param);
    if (!au_gtk.started || au_gtk.quitting || au_gtk.modal || i < 0 || i >= d->list.n)
        return;
    AuCommandState st;
    au_gtk_command_state(&st);
    au_menu_entry_run(&st, au_gtk_command_host(), &d->list.e[i]);
    au_gtk_kick();
}

/* ---- Building ---- */

/* "Open File..." shows as "Open File…". */
static char *label_of(const char *title)
{
    size_t n = strlen(title);
    if (n >= 3 && !strcmp(title + n - 3, "..."))
        return g_strdup_printf("%.*s…", (int)(n - 3), title);
    return g_strdup(title);
}

static void set_accels(AuCommand cmd, const AuShortcut *s)
{
    if (!s->key)
        return;
    char mods[40] = "";
    if (s->mods & AU_SHORTCUT_PRIMARY)
        strcat(mods, "<Control>");
    if (s->mods & AU_SHORTCUT_SHIFT)
        strcat(mods, "<Shift>");
    if (s->mods & AU_SHORTCUT_ALT)
        strcat(mods, "<Alt>");
    char c = s->key >= 'A' && s->key <= 'Z' ? (char)(s->key - 'A' + 'a') : s->key;
    const char *name = gdk_keyval_name(gdk_unicode_to_keyval((guint)(unsigned char)c));
    if (!name)
        return;
    char a[64], b[64], action[32];
    g_snprintf(a, sizeof a, "%s%s", mods, name);
    g_snprintf(action, sizeof action, "app.cmd-%d", (int)cmd);
    const char *accels[3] = {a, NULL, NULL};
    if (c == '+') {
        /* Zoom In: Ctrl and the = key that + shares on most layouts. */
        g_snprintf(b, sizeof b, "%sequal", mods);
        accels[1] = b;
    }
    gtk_application_set_accels_for_action(au_gtk.app, action, accels);
}

static GSimpleAction *command_action(AuCommand cmd)
{
    if (M.cmd[cmd])
        return M.cmd[cmd];
    char name[32];
    g_snprintf(name, sizeof name, "cmd-%d", (int)cmd);
    GSimpleAction *a;
    if (cmd == AU_CMD_VIEW_TABS_TOP || cmd == AU_CMD_VIEW_TABS_LEFT)
        a = g_simple_action_new_stateful(name, NULL, g_variant_new_boolean(FALSE));
    else
        a = g_simple_action_new(name, NULL);
    g_signal_connect(a, "activate", G_CALLBACK(on_cmd), GINT_TO_POINTER((int)cmd));
    g_simple_action_set_enabled(a, FALSE);
    g_action_map_add_action(G_ACTION_MAP(au_gtk.app), G_ACTION(a));
    M.cmd[cmd] = a;
    const AuCommandInfo *ci = au_command_info(cmd);
    if (ci)
        set_accels(cmd, &ci->shortcut);
    return a;
}

static GMenu *build_menu(AuMenuId id)
{
    const AuMenuInfo *mi = au_menu_info(id);
    GMenu *menu = g_menu_new();
    GMenu *section = g_menu_new();
    for (int i = 0; mi && i < mi->nitems; i++) {
        AuCommand cmd = mi->items[i];
        if (cmd == AU_CMD_SEPARATOR) {
            g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
            g_object_unref(section);
            section = g_menu_new();
            continue;
        }
        const AuCommandInfo *ci = au_command_info(cmd);
        if (!ci)
            continue;
        char *label = label_of(ci->title);
        if (ci->submenu != AU_MENU_NONE) {
            GMenu *sub = build_menu(ci->submenu);
            g_menu_append_submenu(section, label, G_MENU_MODEL(sub));
            g_object_unref(sub);
        } else {
            command_action(cmd);
            char action[32];
            g_snprintf(action, sizeof action, "app.cmd-%d", (int)cmd);
            g_menu_append(section, label, action);
        }
        g_free(label);
    }
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
    if (mi && mi->list != AU_MENU_LIST_NONE && M.ndyn < 3) {
        DynList *d = &M.dyn[M.ndyn++];
        d->kind = mi->list;
        d->section = g_menu_new();
        d->action = mi->list == AU_MENU_LIST_HISTORY ? "hist" : mi->list == AU_MENU_LIST_BOOKMARKS ? "bm" : "win";
        g_menu_append_section(menu, NULL, G_MENU_MODEL(d->section));
        GSimpleAction *a = mi->list == AU_MENU_LIST_WINDOWS
                               ? g_simple_action_new_stateful(d->action, G_VARIANT_TYPE_INT32, g_variant_new_int32(-1))
                               : g_simple_action_new(d->action, G_VARIANT_TYPE_INT32);
        g_signal_connect(a, "activate", G_CALLBACK(on_entry), d);
        g_action_map_add_action(G_ACTION_MAP(au_gtk.app), G_ACTION(a));
        g_object_unref(a);
    }
    return menu;
}

void au_gtk_menus_init(void)
{
    if (M.built)
        return;
    M.built = true;
    GSimpleAction *none = g_simple_action_new("none", NULL);
    g_simple_action_set_enabled(none, FALSE);
    g_action_map_add_action(G_ACTION_MAP(au_gtk.app), G_ACTION(none));
    g_object_unref(none);

    GMenu *bar = g_menu_new();
    int n;
    const AuMenuId *ids = au_menu_bar(&n);
    for (int i = 0; i <= n; i++) {
        /* The bar's menus, then Help, which on Linux is the last menu. */
        AuMenuId id = i < n ? ids[i] : AU_MENU_HELP;
        const AuMenuInfo *mi = au_menu_info(id);
        if (!mi)
            continue;
        GMenu *menu = build_menu(id);
        g_menu_append_submenu(bar, mi->title, G_MENU_MODEL(menu));
        g_object_unref(menu);
    }
    gtk_application_set_menubar(au_gtk.app, G_MENU_MODEL(bar));
    g_object_unref(bar);
    M.dirty = true;
}

/* ---- Dynamic lists ---- */

static void fill_list(DynList *d)
{
    g_menu_remove_all(d->section);
    int n = d->list.n;
    if (!n)
        return;
    /* A menu for each folder; separators start a new section. */
    GMenu **menus = g_new0(GMenu *, (gsize)n);
    GMenu *top = g_menu_new();
    int checked = -1;
    for (int i = 0; i < n; i++) {
        const AuMenuEntry *e = &d->list.e[i];
        GMenu *into = e->parent >= 0 && e->parent < i && menus[e->parent] ? menus[e->parent] : top;
        if (e->kind == AU_ENTRY_SEPARATOR) {
            if (into == top && g_menu_model_get_n_items(G_MENU_MODEL(top))) {
                g_menu_append_section(d->section, NULL, G_MENU_MODEL(top));
                g_object_unref(top);
                top = g_menu_new();
            }
            continue;
        }
        char *label = g_strdup(e->title && *e->title ? e->title : " ");
        if (e->kind == AU_ENTRY_FOLDER) {
            menus[i] = g_menu_new();
            g_menu_append_submenu(into, label, G_MENU_MODEL(menus[i]));
        } else {
            GMenuItem *it = g_menu_item_new(label, NULL);
            if (!e->enabled) {
                g_menu_item_set_action_and_target_value(it, "app.none", NULL);
            } else {
                char action[16];
                g_snprintf(action, sizeof action, "app.%s", d->action);
                g_menu_item_set_action_and_target_value(it, action, g_variant_new_int32(i));
            }
            g_menu_append_item(into, it);
            g_object_unref(it);
            if (e->checked)
                checked = i;
        }
        g_free(label);
    }
    if (g_menu_model_get_n_items(G_MENU_MODEL(top)))
        g_menu_append_section(d->section, NULL, G_MENU_MODEL(top));
    g_object_unref(top);
    for (int i = 0; i < n; i++)
        if (menus[i])
            g_object_unref(menus[i]);
    g_free(menus);
    if (d->kind == AU_MENU_LIST_WINDOWS) {
        GAction *a = g_action_map_lookup_action(G_ACTION_MAP(au_gtk.app), d->action);
        if (a)
            g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_int32(checked));
    }
}

/* The browser windows front to back: the front one, then the rest. */
static int window_refs(AuWindowRef *refs, int cap, guint *hash)
{
    int n = 0;
    AuGtkWin *front = au_gtk_front();
    guint h = 5381;
    for (int pass = 0; pass < 2; pass++) {
        for (AuGtkWin *w = au_gtk.wins; w && n < cap; w = w->next) {
            if (w->closing || (pass == 0) != (w == front))
                continue;
            const char *t = gtk_window_get_title(w->window);
            refs[n].title = t ? t : "";
            refs[n].window = w;
            h = h * 33 + g_str_hash(refs[n].title) + (guint)(uintptr_t)w;
            n++;
        }
    }
    *hash = h + (guint)n;
    return n;
}

void au_gtk_menus_dirty(void)
{
    M.dirty = true;
}

/* ---- Refresh ---- */

void au_gtk_menus_refresh(void)
{
    if (!M.built || !au_gtk.browser)
        return;
    AuCommandState st;
    au_gtk_command_state(&st);
    for (int c = 0; c < AU_CMD_COUNT; c++) {
        GSimpleAction *a = M.cmd[c];
        if (!a)
            continue;
        bool on = !au_gtk.modal && resolve(&st, (AuCommand)c) != AU_CMD_NONE &&
                  au_command_enabled(&st, resolve(&st, (AuCommand)c));
        if (g_action_get_enabled(G_ACTION(a)) != on)
            g_simple_action_set_enabled(a, on);
        GVariant *state = g_action_get_state(G_ACTION(a));
        if (state) {
            bool ck = au_command_checked(&st, (AuCommand)c);
            if (g_variant_get_boolean(state) != ck)
                g_simple_action_set_state(a, g_variant_new_boolean(ck));
            g_variant_unref(state);
        }
    }
    AuWindowRef refs[64];
    guint hash;
    int nrefs = window_refs(refs, 64, &hash);
    for (int i = 0; i < M.ndyn; i++) {
        DynList *d = &M.dyn[i];
        if (d->kind == AU_MENU_LIST_WINDOWS) {
            if (hash == M.win_hash && d->list.n)
                continue;
            M.win_hash = hash;
            au_menu_list_free(&d->list);
            au_menu_list_windows(refs, nrefs, nrefs ? refs[0].window : NULL, &d->list);
        } else {
            if (!M.dirty)
                continue;
            au_menu_list_free(&d->list);
            if (d->kind == AU_MENU_LIST_HISTORY)
                au_menu_list_history(au_gtk.browser, &d->list);
            else
                au_menu_list_bookmarks(au_gtk.browser, &d->list);
        }
        fill_list(d);
    }
    M.dirty = false;
}

/* ---- Native windows ----
 * One function per window that AuCommandHost.show_window opens. Each
 * returns whether a window was shown. The Preferences, list, developer
 * and About windows are not in this port yet: they say so and return
 * false. To attach a window, call its show function here. */

static bool not_yet(const char *what)
{
    AuGtkWin *w = au_gtk_front();
    char *msg = g_strdup_printf("%s is not yet available in the GTK port.", what);
    au_gtk_notice(w ? w->window : NULL, msg, NULL);
    g_free(msg);
    return false;
}

static bool show_about(void) { return not_yet("About Aura"); }
static bool show_preferences(void) { return not_yet("Preferences"); }
static bool show_history(void) { return not_yet("The History window"); }
static bool show_bookmarks(void) { return not_yet("The Bookmarks window"); }
static bool show_downloads(void) { return not_yet("The Downloads window"); }
static bool show_cookies(void) { return not_yet("The Cookies window"); }
/* false: the model opens a view-source: tab instead. */
static bool show_page_source(void) { return false; }
static bool show_developer(AuWindowKind kind)
{
    switch (kind) {
    case AU_WINDOW_CONSOLE: return not_yet("The Console");
    case AU_WINDOW_NETWORK: return not_yet("The Network window");
    case AU_WINDOW_PERFORMANCE: return not_yet("The Performance window");
    case AU_WINDOW_SYSTEM_STATUS: return not_yet("System Status");
    case AU_WINDOW_REPORT: return not_yet("Report a Problem");
    case AU_WINDOW_TIMING_TEST: return not_yet("The timing test");
    case AU_WINDOW_PAINT_BENCHMARK: return not_yet("The paint benchmark");
    default: return false;
    }
}

bool au_gtk_show_window(AuWindowKind kind)
{
    switch (kind) {
    case AU_WINDOW_ABOUT: return show_about();
    case AU_WINDOW_PREFERENCES: return show_preferences();
    case AU_WINDOW_HISTORY: return show_history();
    case AU_WINDOW_BOOKMARKS: return show_bookmarks();
    case AU_WINDOW_DOWNLOADS: return show_downloads();
    case AU_WINDOW_COOKIES: return show_cookies();
    case AU_WINDOW_PAGE_SOURCE: return show_page_source();
    default: return show_developer(kind);
    }
}

/* ---- AuCommandHost ---- */

static AuBrowserWindow *h_new_window(void *ctx, const char *url)
{
    (void)ctx;
    AuGtkWin *w = au_gtk_new_window(url, NULL);
    return w ? w->bw : NULL;
}

static void h_raise(void *ctx, AuBrowserWindow *bw)
{
    (void)ctx;
    AuGtkWin *w = au_gtk_win_of(bw);
    if (w)
        gtk_window_present(w->window);
}

static void h_focus_address(void *ctx, AuBrowserWindow *bw)
{
    (void)ctx;
    AuGtkWin *w = au_gtk_win_of(bw);
    if (w)
        au_gtk_win_focus_address(w);
}

static bool h_show_window(void *ctx, AuWindowKind kind)
{
    (void)ctx;
    return au_gtk_show_window(kind);
}

typedef struct FileWait {
    bool done;
    GFile *file;
    bool save;
} FileWait;

static void file_done(GObject *src, GAsyncResult *res, gpointer data)
{
    FileWait *f = data;
    f->file = f->save ? gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL)
                      : gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    f->done = true;
}

static char *h_choose_file_url(void *ctx)
{
    (void)ctx;
    if (au_gtk.modal)
        return NULL;
    AuGtkWin *w = au_gtk_front();
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_modal(d, TRUE);
    FileWait f = {false, NULL, false};
    gtk_file_dialog_open(d, w ? w->window : NULL, NULL, file_done, &f);
    au_gtk_modal_wait(&f.done);
    g_object_unref(d);
    if (!f.file)
        return NULL;
    char *uri = g_file_get_uri(f.file);
    g_object_unref(f.file);
    char *out = uri ? au_strndup(uri, strlen(uri)) : NULL;
    g_free(uri);
    return out;
}

static void h_save_page(void *ctx, AuBrowserWindow *bw)
{
    (void)ctx;
    AuGtkWin *w = au_gtk_win_of(bw);
    if (!w || au_gtk.modal)
        return;
    char name[256];
    au_bw_save_name(bw, name, sizeof name);
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_modal(d, TRUE);
    gtk_file_dialog_set_initial_name(d, name);
    FileWait f = {false, NULL, true};
    gtk_file_dialog_save(d, w->window, NULL, file_done, &f);
    au_gtk_modal_wait(&f.done);
    g_object_unref(d);
    if (!f.file)
        return;
    const char *data;
    size_t len;
    GError *err = NULL;
    if (w->freed || !au_bw_page_source(bw, &data, &len)) {
        g_object_unref(f.file);
        return;
    }
    if (!g_file_replace_contents(f.file, data, len, NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, &err)) {
        au_gtk_notice(w->window, "The page could not be saved.", err ? err->message : NULL);
        g_clear_error(&err);
    }
    g_object_unref(f.file);
}

static void h_close_front(void *ctx)
{
    (void)ctx;
    GtkWindow *act = active_window();
    AuGtkWin *w = au_gtk_front();
    if (act)
        gtk_window_close(act);
    else if (w)
        gtk_window_close(w->window);
}

static void h_find(void *ctx, AuBrowserWindow *bw)
{
    (void)ctx;
    AuGtkWin *w = bw ? au_gtk_win_of(bw) : NULL;
    if (w)
        au_gtk_win_find(w);
}

static void h_tool_find_again(void *ctx, bool forward)
{
    (void)ctx;
    (void)forward; /* no developer windows in this port yet */
}

static void h_edit(void *ctx, AuEditAction a)
{
    (void)ctx;
    au_gtk_edit(a);
}

static void h_copy_text(void *ctx, const char *utf8, size_t len)
{
    (void)ctx;
    au_gtk_copy_text(utf8, len);
}

static void h_select_window(void *ctx, void *window)
{
    (void)ctx;
    for (AuGtkWin *w = au_gtk.wins; w; w = w->next)
        if (w == window)
            gtk_window_present(w->window);
}

static void h_beep(void *ctx)
{
    (void)ctx;
    AuGtkWin *w = au_gtk_front();
    if (w)
        gtk_widget_error_bell(GTK_WIDGET(w->window));
}

static gboolean quit_idle(gpointer data)
{
    (void)data;
    au_gtk_quit();
    return G_SOURCE_REMOVE;
}

static void h_quit(void *ctx)
{
    (void)ctx;
    /* After the command returns: au_gtk_quit frees the model. */
    g_idle_add(quit_idle, NULL);
}

static const AuCommandHost command_host = {
    NULL,          h_new_window,  h_raise,     h_focus_address,  h_show_window,
    h_choose_file_url, h_save_page, h_close_front, h_find,      h_tool_find_again,
    h_edit,        h_copy_text,   h_select_window, h_beep,      h_quit};

const AuCommandHost *au_gtk_command_host(void)
{
    return &command_host;
}

/* ---- Editing ---- */

static GtkWidget *focused_text(AuGtkWin *w)
{
    GtkWindow *act = active_window();
    GtkWidget *focus = act ? gtk_root_get_focus(GTK_ROOT(act)) : NULL;
    if (focus && GTK_IS_TEXT(focus))
        return focus;
    (void)w;
    return NULL;
}

typedef enum { PASTE_PAGE, PASTE_GO } PasteKind;

static void paste_done(GObject *src, GAsyncResult *res, gpointer data)
{
    PasteKind kind = (PasteKind)GPOINTER_TO_INT(data);
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    AuGtkWin *w = au_gtk_front();
    if (!text || !w || w->freed || au_gtk.quitting) {
        g_free(text);
        return;
    }
    if (kind == PASTE_PAGE) {
        AuBuf out;
        au_buf_init(&out);
        au_bw_page_edit(w->bw, AU_PAGE_EDIT_PASTE, text, strlen(text), &out);
        au_buf_free(&out);
    } else {
        /* Paste and Go: one line, trimmed, opened from the page. */
        for (char *p = text; *p; p++)
            if (*p == '\r' || *p == '\n' || *p == '\t')
                *p = ' ';
        g_strstrip(text);
        if (*text) {
            au_bw_set_field_text(w->bw, NULL);
            gtk_widget_grab_focus(w->area);
            au_bw_navigate_input(w->bw, text);
        }
    }
    g_free(text);
    au_gtk_kick();
}

static void read_clipboard(PasteKind kind)
{
    GdkDisplay *d = gdk_display_get_default();
    if (d)
        gdk_clipboard_read_text_async(gdk_display_get_clipboard(d), NULL, paste_done, GINT_TO_POINTER((int)kind));
}

void au_gtk_edit(AuEditAction a)
{
    AuGtkWin *w = au_gtk_front();
    if (a == AU_EDIT_PASTE_AND_GO) {
        if (w)
            read_clipboard(PASTE_GO);
        return;
    }
    GtkWidget *text = focused_text(w);
    if (text) {
        /* The address field, the find field, or a dialog's field. */
        static const char *const actions[] = {"text.undo",        "clipboard.cut",  "clipboard.copy",
                                              "clipboard.paste",  "selection.delete", "selection.select-all"};
        if ((size_t)a < sizeof actions / sizeof actions[0])
            gtk_widget_activate_action(text, actions[a], NULL);
        return;
    }
    if (!w || w->freed)
        return;
    if (au_bw_page_editing(w->bw)) {
        static const int page_cmd[] = {AU_PAGE_EDIT_UNDO,  AU_PAGE_EDIT_CUT,   AU_PAGE_EDIT_COPY,
                                       AU_PAGE_EDIT_PASTE, AU_PAGE_EDIT_CLEAR, AU_PAGE_EDIT_SELECT_ALL};
        if (a == AU_EDIT_PASTE) {
            read_clipboard(PASTE_PAGE);
            return;
        }
        if ((size_t)a >= sizeof page_cmd / sizeof page_cmd[0])
            return;
        AuBuf out;
        au_buf_init(&out);
        if (au_bw_page_edit(w->bw, page_cmd[a], NULL, 0, &out) && out.len &&
            (a == AU_EDIT_CUT || a == AU_EDIT_COPY))
            au_gtk_copy_text(out.data, out.len);
        au_buf_free(&out);
    } else if (a == AU_EDIT_COPY) {
        AuBuf out;
        au_buf_init(&out);
        if (au_bw_selection_text(w->bw, &out) && out.len)
            au_gtk_copy_text(out.data, out.len);
        au_buf_free(&out);
    } else if (a == AU_EDIT_SELECT_ALL) {
        au_bw_select_all(w->bw);
    }
    au_gtk_kick();
}

/* ---- Contextual menus ---- */

static void on_context(GSimpleAction *a, GVariant *param, gpointer data)
{
    AuGtkWin *w = data;
    (void)a;
    if (w->freed || !w->have_menu || au_gtk.modal || !au_gtk.started || au_gtk.quitting)
        return;
    AuCommandState st;
    au_gtk_command_state(&st);
    st.window = w->bw;
    au_command_run_context(&st, au_gtk_command_host(), &w->menu, (AuCommand)g_variant_get_int32(param));
    au_gtk_kick();
}

void au_gtk_context_menu(AuGtkWin *w, double x, double y)
{
    if (w->have_menu) {
        au_context_menu_free(&w->menu);
        w->have_menu = false;
    }
    if (w->menu_popover) {
        gtk_widget_unparent(w->menu_popover);
        w->menu_popover = NULL;
    }
    unsigned flags = 0;
    GdkDisplay *d = gtk_widget_get_display(w->area);
    GdkContentFormats *f = gdk_clipboard_get_formats(gdk_display_get_clipboard(d));
    if (f && gdk_content_formats_contain_gtype(f, G_TYPE_STRING))
        flags |= AU_CONTEXT_CLIPBOARD_TEXT;
    memset(&w->menu, 0, sizeof w->menu);
    if (!au_bw_context_menu(w->bw, (int)x, (int)y, flags, &w->menu) || !w->menu.n) {
        au_context_menu_free(&w->menu);
        return;
    }
    w->have_menu = true;
    AuCommandState st;
    au_gtk_command_state(&st);
    st.window = w->bw;

    GMenu *menu = g_menu_new();
    GMenu *section = g_menu_new();
    for (int i = 0; i < w->menu.n; i++) {
        const AuContextItem *it = &w->menu.items[i];
        if (it->cmd == AU_CMD_SEPARATOR) {
            if (g_menu_model_get_n_items(G_MENU_MODEL(section))) {
                g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
                g_object_unref(section);
                section = g_menu_new();
            }
            continue;
        }
        const char *title = it->label ? it->label : au_command_title(&st, it->cmd);
        char *label = label_of(title ? title : "");
        GMenuItem *mi = g_menu_item_new(label, NULL);
        if (it->enabled)
            g_menu_item_set_action_and_target_value(mi, "ctx.run", g_variant_new_int32((int32_t)it->cmd));
        else
            g_menu_item_set_action_and_target_value(mi, "app.none", NULL);
        g_menu_append_item(section, mi);
        g_object_unref(mi);
        g_free(label);
    }
    if (g_menu_model_get_n_items(G_MENU_MODEL(section)))
        g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);

    GSimpleActionGroup *group = g_simple_action_group_new();
    GSimpleAction *run = g_simple_action_new("run", G_VARIANT_TYPE_INT32);
    g_signal_connect(run, "activate", G_CALLBACK(on_context), w);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(run));
    g_object_unref(run);
    gtk_widget_insert_action_group(w->area, "ctx", G_ACTION_GROUP(group));
    g_object_unref(group);

    GtkWidget *po = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(menu);
    gtk_widget_set_parent(po, w->area);
    GdkRectangle r = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(po), &r);
    gtk_popover_set_position(GTK_POPOVER(po), GTK_POS_BOTTOM);
    gtk_popover_set_has_arrow(GTK_POPOVER(po), FALSE);
    gtk_widget_set_halign(po, GTK_ALIGN_START);
    w->menu_popover = po;
    gtk_popover_popup(GTK_POPOVER(po));
}
