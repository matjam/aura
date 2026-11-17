/* The About box: a GtkAboutDialog filled from the shared credits
 * (src/browser/credits.h) and the build's identity (sysinfo.h). Links
 * open in Aura, in a new tab, rather than in another browser. */
#include "winutil_gtk.h"

#include "browser/credits.h"

static GtkWidget *g_about;

static gboolean about_link(GtkAboutDialog *d, const char *uri, gpointer data)
{
    (void)d;
    (void)data;
    if (!au_gtk_host.open_url)
        return FALSE; /* the system's handler */
    au_gtk_host.open_url(au_gtk_host.ctx, uri, true);
    return TRUE;
}

static void about_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    g_about = NULL;
    au_gtk_windows_changed();
}

/* One credit section per kind: the names, in the table's order. */
static void add_section(GtkAboutDialog *d, const char *title, AuCreditKind kind)
{
    const char **names = g_new0(const char *, au_ncredits + 1);
    size_t n = 0;
    for (size_t i = 0; i < au_ncredits; i++)
        if (au_credits[i].kind == kind)
            names[n++] = au_credits[i].name;
    if (n)
        gtk_about_dialog_add_credit_section(d, title, names);
    g_free(names);
}

void au_gtk_about_show(void)
{
    if (g_about) {
        gtk_window_present(GTK_WINDOW(g_about));
        return;
    }
    GtkAboutDialog *d = GTK_ABOUT_DIALOG(gtk_about_dialog_new());
    g_about = GTK_WIDGET(d);
    gtk_about_dialog_set_program_name(d, "Aura");
    gtk_about_dialog_set_version(d, au_build_identity());
    gtk_about_dialog_set_comments(d, AU_TAGLINE);
    gtk_about_dialog_set_website(d, AU_HOMEPAGE);
    gtk_about_dialog_set_website_label(d, "Aura on GitHub");
    gtk_about_dialog_set_copyright(d, "Written by " AU_AUTHOR ", with help from Claude.");
    gtk_about_dialog_set_license_type(d, GTK_LICENSE_MIT_X11);
    static const char *authors[] = {AU_AUTHOR, NULL};
    gtk_about_dialog_set_authors(d, authors);
    add_section(d, "Libraries", AU_CREDIT_LIBRARY);
    add_section(d, "Fonts", AU_CREDIT_FONT);
    add_section(d, "Data", AU_CREDIT_DATA);
    gtk_about_dialog_set_system_information(d, "The licences of the libraries, fonts and data Aura includes are "
                                               "on the page about:licences.");
    const char *id = au_gtk_host.app ? g_application_get_application_id(G_APPLICATION(au_gtk_host.app)) : NULL;
    gtk_about_dialog_set_logo_icon_name(d, id ? id : "web-browser");
    GtkWindow *parent = au_gtk_front_parent();
    if (parent)
        gtk_window_set_transient_for(GTK_WINDOW(d), parent);
    if (au_gtk_host.app)
        gtk_window_set_application(GTK_WINDOW(d), au_gtk_host.app);
    g_signal_connect(d, "activate-link", G_CALLBACK(about_link), NULL);
    g_signal_connect(d, "destroy", G_CALLBACK(about_destroy), NULL);
    gtk_window_set_hide_on_close(GTK_WINDOW(d), FALSE);
    gtk_window_present(GTK_WINDOW(d));
    au_gtk_windows_changed();
}

void au_gtk_about_close(void)
{
    if (g_about)
        gtk_window_destroy(GTK_WINDOW(g_about));
}
