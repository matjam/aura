/* The GTK4 application: start, the tick, quit and session restore
 * (docs/gtk.md). The order follows src/browser/app.h and the Mac OS 9
 * shell: fonts, the browser, the profile files, the network, then the
 * windows; at quit the reverse. */
#include "gtk.h"
#include "windows_gtk.h"

#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#define AURA_APP_ID "org.aura.Browser"

/* The tick: every 20 ms, or 10 ms while a page, a download or the
 * network is busy. au_gtk_kick runs a flush sooner (after input, an
 * invalidated rectangle). */
#define TICK_IDLE_MS 20
#define TICK_BUSY_MS 10

static guint tick_id, kick_id;

static void flush_all(void)
{
    AuGtkWin *w = au_gtk.wins;
    while (w) {
        AuGtkWin *next = w->next;
        if (w->closing)
            au_gtk_win_destroy(w);
        else
            au_gtk_win_flush(w);
        w = next;
    }
    /* The model closed the last window's last tab. */
    if (!au_gtk.wins && au_gtk.started && !au_gtk.quitting)
        au_gtk_quit();
}

static bool busy(void)
{
    return au_browser_busy(au_gtk.browser) || au_downloads_busy(&au_gtk.browser->downloads) ||
           au_app_net_busy(au_gtk.a);
}

static void schedule_tick(guint ms);

static gboolean tick(gpointer data)
{
    (void)data;
    tick_id = 0;
    if (au_gtk.quitting)
        return G_SOURCE_REMOVE;
    /* A dialog the model waits for runs a nested loop: nothing may enter
     * the model until it answers. */
    if (!au_gtk.modal) {
        au_gtk_net_pump();
        au_downloads_idle(&au_gtk.browser->downloads, au_posix_ms());
        for (AuGtkWin *w = au_gtk.wins; w; w = w->next)
            if (!w->closing)
                au_bw_idle(w->bw);
        flush_all();
        if (au_gtk.quitting)
            return G_SOURCE_REMOVE;
        au_gtk_menus_refresh();
        au_app_save(au_gtk.a, false);
    }
    schedule_tick(busy() ? TICK_BUSY_MS : TICK_IDLE_MS);
    return G_SOURCE_REMOVE;
}

static void schedule_tick(guint ms)
{
    if (tick_id)
        g_source_remove(tick_id);
    tick_id = g_timeout_add(ms, tick, NULL);
}

static gboolean kick(gpointer data)
{
    (void)data;
    kick_id = 0;
    if (!au_gtk.modal && !au_gtk.quitting)
        flush_all();
    return G_SOURCE_REMOVE;
}

void au_gtk_kick(void)
{
    if (!au_gtk.started || au_gtk.quitting || kick_id)
        return;
    /* Before GTK's own drawing (G_PRIORITY_HIGH_IDLE + 20). */
    kick_id = g_idle_add_full(G_PRIORITY_HIGH_IDLE, kick, NULL, NULL);
}

AuGtkWin *au_gtk_new_window(const char *url, const AuSessionWindow *sw)
{
    AuPrefs *p = &au_gtk.browser->prefs;
    int width = p->window_width >= 300 ? p->window_width : 1024;
    int height = p->window_height >= 200 ? p->window_height : 700;
    if (sw && sw->width >= 300 && sw->height >= 200) {
        width = sw->width;
        height = sw->height;
    }
    AuGtkWin *w = au_gtk_win_new(width, height);
    if (!w)
        return NULL;
    if (sw && sw->ntabs)
        au_bw_restore(w->bw, sw);
    else
        au_bw_new_tab(w->bw, url ? url : p->home_page, true, -1);
    gtk_window_present(w->window);
    return w;
}

void au_gtk_quit(void)
{
    if (au_gtk.quitting || !au_gtk.started)
        return;
    au_gtk.quitting = true;
    if (tick_id)
        g_source_remove(tick_id);
    if (kick_id)
        g_source_remove(kick_id);
    tick_id = kick_id = 0;
    /* The secondary windows close first: an open Preferences window is
     * cancelled, so the last save has the preferences from before it. */
    au_gtk_windows_shutdown();
    /* Privacy options, the last save (the session while the windows are
     * open) and the Running file; then the windows; then the network. */
    au_app_quit(au_gtk.a);
    while (au_gtk.wins)
        au_gtk_win_destroy(au_gtk.wins);
    au_gtk_net_stop();
    au_gtk_appearance_stop();
    au_app_free(au_gtk.a);
    au_gtk.a = NULL;
    au_browser_free(au_gtk.browser);
    au_gtk.browser = NULL;
    au_fontset_free(au_gtk.fonts);
    au_gtk.fonts = NULL;
    au_gtk_fonts_free();
    g_application_quit(G_APPLICATION(au_gtk.app));
}

static gboolean on_signal(gpointer data)
{
    (void)data;
    /* SIGTERM and SIGINT quit as the menu does, so the session is saved. */
    au_gtk_quit();
    if (!au_gtk.started)
        g_application_quit(G_APPLICATION(au_gtk.app));
    return G_SOURCE_CONTINUE;
}

/* Fonts, the browser, the files, the network, the first windows. */
static void start(const char *const *urls, int nurls)
{
    au_gtk.fonts = au_fontset_new();
    au_platform_load_fonts(au_gtk.fonts);
    AuBrowserHost host;
    au_gtk_browser_host(&host);
    au_gtk.browser = au_browser_new(au_gtk.fonts, &host);
    au_gtk.browser->site_icons = true;
    AuAppPort port;
    bool files = au_gtk_files_init(&port);
    au_gtk.a = au_app_new(au_gtk.browser, &port);
    bool unclean = files && au_app_load(au_gtk.a);
    au_gtk_appearance_start();
    if (!au_gtk_net_start())
        fprintf(stderr, "aura: the network could not be started\n");
    au_gtk.started = true;
    au_gtk_windows_start();

    /* The last tabs come back when the preference says so or, after a
     * crash, when the user says so. */
    AuSession session;
    bool restored = false;
    AuAppRestore restore = au_app_session(au_gtk.a, unclean, &session);
    bool have = restore != AU_APP_RESTORE_NONE;
    if (restore == AU_APP_RESTORE_ASK &&
        !au_gtk_ask(NULL, "Aura did not quit normally last time.", "Reopen the windows and tabs from then?",
                    "Reopen", "Start New")) {
        au_session_free(&session);
        have = false;
    }
    if (have) {
        for (int i = 0; i < session.nwindows; i++) {
            if (session.windows[i].ntabs && au_gtk_new_window(NULL, &session.windows[i]))
                restored = true;
        }
        au_session_free(&session);
    }
    if (nurls) {
        /* URLs from the command line: tabs of the front window, or a new one. */
        AuGtkWin *w = au_gtk_front();
        for (int i = 0; i < nurls; i++) {
            if (w)
                au_bw_new_tab(w->bw, urls[i], true, -1);
            else
                w = au_gtk_new_window(urls[i], NULL);
        }
        restored = restored || w;
    }
    if (!restored)
        au_gtk_new_window(au_app_start_url(au_gtk.a), NULL);
    schedule_tick(TICK_BUSY_MS);
}

static void on_startup(GApplication *app, gpointer data)
{
    (void)app;
    (void)data;
    au_gtk_menus_init();
}

static void on_activate(GApplication *app, gpointer data)
{
    (void)data;
    if (au_gtk.quitting)
        return;
    g_application_hold(app);
    if (!au_gtk.started)
        start(NULL, 0);
    else
        au_gtk_new_window(NULL, NULL);
    g_application_release(app);
}

/* The command line's own arguments. GIO turns an argument such as
 * about:home into the URI about:///home, so an argument with a scheme
 * other than file: is passed on as it was typed. */
static char **raw_args;
static int raw_n;

static bool has_scheme(const char *s)
{
    if (!g_ascii_isalpha(s[0]))
        return false;
    const char *p = s + 1;
    while (g_ascii_isalnum(*p) || *p == '+' || *p == '-' || *p == '.')
        p++;
    return *p == ':' && g_ascii_strncasecmp(s, "file:", 5) != 0;
}

static char *open_url(GFile *file, gint i, gint n)
{
    if (raw_n == n && has_scheme(raw_args[i]) &&
        !g_file_test(raw_args[i], G_FILE_TEST_EXISTS))
        return g_strdup(raw_args[i]);
    return g_file_get_uri(file);
}

static void on_open(GApplication *app, GFile **files, gint n, const gchar *hint, gpointer data)
{
    (void)hint;
    (void)data;
    if (au_gtk.quitting)
        return;
    char **urls = g_new0(char *, (gsize)n + 1);
    for (gint i = 0; i < n; i++)
        urls[i] = open_url(files[i], i, n);
    g_application_hold(app);
    if (!au_gtk.started) {
        start((const char *const *)urls, n);
    } else {
        AuGtkWin *w = au_gtk_front();
        for (gint i = 0; i < n; i++) {
            if (w)
                au_bw_new_tab(w->bw, urls[i], true, -1);
            else
                w = au_gtk_new_window(urls[i], NULL);
        }
        if (w)
            gtk_window_present(w->window);
    }
    g_application_release(app);
    g_strfreev(urls);
}

static void on_shutdown(GApplication *app, gpointer data)
{
    (void)app;
    (void)data;
    au_gtk_quit();
}

int main(int argc, char **argv)
{
    au_gtk_find_assets(argv[0]);
    raw_args = g_new0(char *, (gsize)argc + 1);
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-')
            raw_args[raw_n++] = argv[i];
    /* One process per launch: the profile is used by that process only
     * (docs/gtk.md, "Known gaps"). */
    au_gtk.app = gtk_application_new(AURA_APP_ID, G_APPLICATION_NON_UNIQUE | G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(au_gtk.app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(au_gtk.app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(au_gtk.app, "open", G_CALLBACK(on_open), NULL);
    g_signal_connect(au_gtk.app, "shutdown", G_CALLBACK(on_shutdown), NULL);
    g_unix_signal_add(SIGTERM, on_signal, NULL);
    g_unix_signal_add(SIGINT, on_signal, NULL);
    int status = g_application_run(G_APPLICATION(au_gtk.app), argc, argv);
    g_object_unref(au_gtk.app);
    return status;
}
