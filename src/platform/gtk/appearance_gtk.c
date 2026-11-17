/* The Appearance preference on GTK (docs/gtk.md, "Appearance").
 *
 * The desktop's light or dark setting decides the scheme while the
 * preference is System. It is read from the settings portal
 * (org.freedesktop.portal.Settings, org.freedesktop.appearance
 * color-scheme) over GDBus, and followed through its SettingChanged
 * signal. Without a portal, GtkSettings:gtk-interface-color-scheme (GTK
 * 4.20 and later) is read when the running GTK has it.
 *
 * The chrome follows the preference: Light and Dark force the theme's
 * variant through gtk-application-prefer-dark-theme (and
 * gtk-interface-color-scheme where it exists); System puts back what the
 * desktop set. The properties are set by name so the build against GTK
 * 4.12 headers and the run on a newer GTK both work without deprecation
 * warnings. */
#include "gtk.h"

/* The portal's values: 0 no preference, 1 prefer dark, 2 prefer light. */
#define PORTAL_DARK 1u
/* GtkInterfaceColorScheme: 0 unsupported, 1 default, 2 dark, 3 light. */
#define ICS_DARK 2
#define ICS_LIGHT 3

static struct {
    bool started;
    GDBusConnection *bus;
    guint signal_id;
    bool portal; /* the portal answered: it decides, not GtkSettings */
    bool has_ics;
    /* The desktop's values, from before Aura set either. */
    gboolean orig_prefer_dark;
    int orig_ics;
    bool forced; /* Aura set the properties (Light or Dark) */
    bool writing; /* our own set: ignore the notify it causes */
} ap;

static GtkSettings *settings(void)
{
    return gtk_settings_get_default();
}

static void set_system_dark(bool dark)
{
    if (!au_gtk.browser || au_gtk.browser->system_dark == dark)
        return;
    au_browser_set_system_dark(au_gtk.browser, dark);
    au_gtk_kick();
}

/* A portal value is a uint32 inside one or two variants (Read wraps it
 * once more than ReadOne). */
static bool portal_value(GVariant *v, guint32 *out)
{
    GVariant *cur = g_variant_ref(v);
    for (int depth = 0; depth < 4 && g_variant_is_of_type(cur, G_VARIANT_TYPE_VARIANT); depth++) {
        GVariant *inner = g_variant_get_variant(cur);
        g_variant_unref(cur);
        cur = inner;
    }
    bool ok = g_variant_is_of_type(cur, G_VARIANT_TYPE_UINT32);
    if (ok)
        *out = g_variant_get_uint32(cur);
    g_variant_unref(cur);
    return ok;
}

static void on_setting_changed(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                               const char *signal, GVariant *params, gpointer data)
{
    (void)c, (void)sender, (void)path, (void)iface, (void)signal, (void)data;
    const char *ns = NULL, *key = NULL;
    GVariant *value = NULL;
    if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(ssv)")))
        return;
    g_variant_get(params, "(&s&sv)", &ns, &key, &value);
    guint32 u;
    if (!strcmp(ns, "org.freedesktop.appearance") && !strcmp(key, "color-scheme") && portal_value(value, &u)) {
        ap.portal = true;
        set_system_dark(u == PORTAL_DARK);
    }
    g_variant_unref(value);
}

/* Without a portal: the running GTK's own idea of the desktop. */
static void read_ics(void)
{
    if (ap.has_ics && !ap.portal)
        set_system_dark(ap.orig_ics == ICS_DARK);
}

static void on_ics_notify(GObject *o, GParamSpec *ps, gpointer data)
{
    (void)o, (void)ps, (void)data;
    if (ap.writing || ap.forced)
        return;
    g_object_get(settings(), "gtk-interface-color-scheme", &ap.orig_ics, NULL);
    read_ics();
}

static void on_prefer_dark_notify(GObject *o, GParamSpec *ps, gpointer data)
{
    (void)o, (void)ps, (void)data;
    /* The desktop changed it while System: remember it to put back. */
    if (!ap.writing && !ap.forced)
        g_object_get(settings(), "gtk-application-prefer-dark-theme", &ap.orig_prefer_dark, NULL);
}

static void on_read(GObject *src, GAsyncResult *res, gpointer data)
{
    bool read_one = GPOINTER_TO_INT(data) != 0;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!r) {
        g_clear_error(&err);
        if (read_one) {
            /* An older portal: Read, the deprecated call. */
            g_dbus_connection_call(ap.bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                   "org.freedesktop.portal.Settings", "Read",
                                   g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"), NULL,
                                   G_DBUS_CALL_FLAGS_NONE, 2000, NULL, on_read, GINT_TO_POINTER(0));
        } else {
            read_ics();
        }
        return;
    }
    GVariant *v = g_variant_get_child_value(r, 0);
    guint32 u;
    if (portal_value(v, &u)) {
        ap.portal = true;
        set_system_dark(u == PORTAL_DARK);
    } else {
        read_ics();
    }
    g_variant_unref(v);
    g_variant_unref(r);
}

static void on_bus(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src, (void)data;
    ap.bus = g_bus_get_finish(res, NULL);
    if (!ap.bus) {
        read_ics();
        return;
    }
    ap.signal_id = g_dbus_connection_signal_subscribe(
        ap.bus, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Settings", "SettingChanged",
        "/org/freedesktop/portal/desktop", "org.freedesktop.appearance", G_DBUS_SIGNAL_FLAGS_NONE, on_setting_changed,
        NULL, NULL);
    g_dbus_connection_call(ap.bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                           "org.freedesktop.portal.Settings", "ReadOne",
                           g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"), NULL,
                           G_DBUS_CALL_FLAGS_NONE, 2000, NULL, on_read, GINT_TO_POINTER(1));
}

void au_gtk_appearance_apply(void)
{
    GtkSettings *s = settings();
    if (!s || !au_gtk.browser)
        return;
    AuColorScheme pref = au_gtk.browser->prefs.color_scheme;
    ap.writing = true;
    if (pref == AU_SCHEME_SYSTEM) {
        if (ap.forced) {
            g_object_set(s, "gtk-application-prefer-dark-theme", ap.orig_prefer_dark, NULL);
            if (ap.has_ics)
                g_object_set(s, "gtk-interface-color-scheme", ap.orig_ics, NULL);
            ap.forced = false;
        }
    } else {
        bool dark = pref == AU_SCHEME_DARK;
        g_object_set(s, "gtk-application-prefer-dark-theme", (gboolean)dark, NULL);
        if (ap.has_ics)
            g_object_set(s, "gtk-interface-color-scheme", dark ? ICS_DARK : ICS_LIGHT, NULL);
        ap.forced = true;
    }
    ap.writing = false;
}

void au_gtk_appearance_start(void)
{
    GtkSettings *s = settings();
    if (ap.started || !s)
        return;
    ap.started = true;
    GObjectClass *k = G_OBJECT_GET_CLASS(s);
    g_object_get(s, "gtk-application-prefer-dark-theme", &ap.orig_prefer_dark, NULL);
    g_signal_connect(s, "notify::gtk-application-prefer-dark-theme", G_CALLBACK(on_prefer_dark_notify), NULL);
    GParamSpec *ps = g_object_class_find_property(k, "gtk-interface-color-scheme");
    ap.has_ics = ps && (ps->flags & G_PARAM_WRITABLE);
    if (ap.has_ics) {
        g_object_get(s, "gtk-interface-color-scheme", &ap.orig_ics, NULL);
        g_signal_connect(s, "notify::gtk-interface-color-scheme", G_CALLBACK(on_ics_notify), NULL);
    }
    au_gtk_appearance_apply();
    g_bus_get(G_BUS_TYPE_SESSION, NULL, on_bus, NULL);
}

void au_gtk_appearance_stop(void)
{
    if (ap.bus) {
        if (ap.signal_id)
            g_dbus_connection_signal_unsubscribe(ap.bus, ap.signal_id);
        g_clear_object(&ap.bus);
    }
    ap.signal_id = 0;
}
