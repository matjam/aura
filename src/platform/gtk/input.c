/* Input on the page: keys and the input method, the mouse, and scrolling
 * (docs/gtk.md, "Input"). Each becomes an AuEvent for au_bw_key or
 * au_bw_mouse, in the page surface's logical pixels.
 *
 * Modifiers: Ctrl is the primary modifier, AU_MOD_COMMAND, as Command is
 * on the Mac (Ctrl-click opens a link in a new tab; text editing skips
 * keys with it). Ctrl with an arrow, Backspace or Delete is
 * AU_MOD_OPTION instead, which the text fields read as "by word", the
 * Linux meaning of those keys. Alt is AU_MOD_OPTION. */
#include "gtk.h"

#include <string.h>

uint32_t au_gtk_mods(GdkModifierType state)
{
    uint32_t m = 0;
    if (state & GDK_SHIFT_MASK)
        m |= AU_MOD_SHIFT;
    if (state & GDK_CONTROL_MASK)
        m |= AU_MOD_COMMAND;
    if (state & GDK_ALT_MASK)
        m |= AU_MOD_OPTION;
    return m;
}

uint32_t au_gtk_current_mods(GtkWidget *widget)
{
    GdkDisplay *d = widget ? gtk_widget_get_display(widget) : gdk_display_get_default();
    GdkSeat *seat = d ? gdk_display_get_default_seat(d) : NULL;
    GdkDevice *kb = seat ? gdk_seat_get_keyboard(seat) : NULL;
    return kb ? au_gtk_mods(gdk_device_get_modifier_state(kb)) : 0;
}

static void send_key(AuGtkWin *w, int key, uint32_t ch, uint32_t mods, bool *handled)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_KEY_DOWN;
    e.time_ms = au_posix_ms();
    e.key = key;
    e.ch = ch;
    e.mods = mods;
    bool h = au_bw_key(w->bw, &e);
    if (!h && key == AU_KEY_ESCAPE) {
        au_bw_stop(w->bw); /* as the Mac does: Escape the page did not take stops the load */
        h = true;
    }
    if (handled)
        *handled = h;
    au_gtk_kick();
}

/* Text from the input method (plain keys, dead keys, compose sequences,
 * and the input method's own composition). */
static void on_commit(GtkIMContext *im, const char *text, gpointer data)
{
    AuGtkWin *w = data;
    (void)im;
    if (w->freed || au_gtk.modal)
        return;
    uint32_t mods = au_gtk_current_mods(w->area) & AU_MOD_SHIFT;
    for (const char *p = text; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (c == '\n' || c == '\r')
            send_key(w, AU_KEY_RETURN, 0, mods, NULL);
        else if (c == '\t')
            send_key(w, AU_KEY_TAB, 0, mods, NULL);
        else if (c >= 0x20 && c != 0x7F)
            send_key(w, 0, c, mods, NULL);
    }
}

static int special_key(guint keyval)
{
    switch (keyval) {
    case GDK_KEY_Left: case GDK_KEY_KP_Left: return AU_KEY_LEFT;
    case GDK_KEY_Right: case GDK_KEY_KP_Right: return AU_KEY_RIGHT;
    case GDK_KEY_Up: case GDK_KEY_KP_Up: return AU_KEY_UP;
    case GDK_KEY_Down: case GDK_KEY_KP_Down: return AU_KEY_DOWN;
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: return AU_KEY_PAGE_UP;
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: return AU_KEY_PAGE_DOWN;
    case GDK_KEY_Home: case GDK_KEY_KP_Home: return AU_KEY_HOME;
    case GDK_KEY_End: case GDK_KEY_KP_End: return AU_KEY_END;
    case GDK_KEY_BackSpace: return AU_KEY_BACKSPACE;
    case GDK_KEY_Delete: case GDK_KEY_KP_Delete: return AU_KEY_DELETE;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_ISO_Enter: return AU_KEY_RETURN;
    case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: case GDK_KEY_KP_Tab: return AU_KEY_TAB;
    case GDK_KEY_Escape: return AU_KEY_ESCAPE;
    default: return AU_KEY_NONE;
    }
}

static gboolean on_key(GtkEventControllerKey *k, guint keyval, guint code, GdkModifierType state, gpointer data)
{
    AuGtkWin *w = data;
    (void)k;
    (void)code;
    if (w->freed)
        return FALSE;
    if (au_gtk.modal)
        return TRUE;
    uint32_t mods = au_gtk_mods(state);
    /* Ctrl-1 to Ctrl-8 choose a tab, Ctrl-9 the last. */
    if ((state & GDK_CONTROL_MASK) && !(state & (GDK_ALT_MASK | GDK_SHIFT_MASK)) && keyval >= GDK_KEY_1 &&
        keyval <= GDK_KEY_9) {
        int n = au_bw_tab_count(w->bw);
        int i = keyval == GDK_KEY_9 ? n - 1 : (int)(keyval - GDK_KEY_1);
        if (i >= 0 && i < n)
            au_bw_select_tab(w->bw, i);
        au_gtk_kick();
        return TRUE;
    }
    int key = special_key(keyval);
    if (key == AU_KEY_NONE) {
        /* Characters come through the input method's commit; a chord
         * with Ctrl or Alt goes on to the menu accelerators. */
        return FALSE;
    }
    if (mods & AU_MOD_COMMAND) {
        switch (key) {
        case AU_KEY_LEFT:
        case AU_KEY_RIGHT:
        case AU_KEY_UP:
        case AU_KEY_DOWN:
        case AU_KEY_BACKSPACE:
        case AU_KEY_DELETE: mods = (mods & ~(uint32_t)AU_MOD_COMMAND) | AU_MOD_OPTION; break;
        default: break;
        }
    }
    bool handled;
    send_key(w, key, 0, mods, &handled);
    /* Tab the page did not take stays on the page rather than moving the
     * keyboard focus to the header bar. */
    return handled || key == AU_KEY_TAB;
}

static void on_page_focus_in(GtkEventControllerFocus *f, gpointer data)
{
    GtkIMContext *im = data;
    (void)f;
    gtk_im_context_focus_in(im);
}

static void on_page_focus_out(GtkEventControllerFocus *f, gpointer data)
{
    GtkIMContext *im = data;
    (void)f;
    gtk_im_context_focus_out(im);
}

/* ---- The mouse ----
 * Buttons and motion come from a legacy controller so that every press
 * has its release, a drag included; the click count follows GTK's
 * double-click time and distance. */

static void send_mouse(AuGtkWin *w, AuEventType type, double x, double y, int button, uint32_t mods)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.time_ms = au_posix_ms();
    e.x = (int)x;
    e.y = (int)y;
    e.button = button;
    e.clicks = w->clicks ? w->clicks : 1;
    e.mods = mods;
    au_bw_mouse(w->bw, &e);
    au_gtk_kick();
}

static int click_count(AuGtkWin *w, double x, double y, int button)
{
    GtkSettings *s = gtk_widget_get_settings(w->area);
    int time = 400, dist = 5;
    g_object_get(s, "gtk-double-click-time", &time, "gtk-double-click-distance", &dist, NULL);
    uint32_t now = au_posix_ms();
    double dx = x - w->press_x, dy = y - w->press_y;
    if (w->clicks && button == w->press_button && now - w->press_ms <= (uint32_t)time &&
        dx * dx + dy * dy <= (double)dist * dist)
        w->clicks = w->clicks < 3 ? w->clicks + 1 : 1;
    else
        w->clicks = 1;
    w->press_ms = now;
    w->press_x = x;
    w->press_y = y;
    w->press_button = button;
    return w->clicks;
}

static bool page_point(AuGtkWin *w, GdkEvent *ev, double *x, double *y)
{
    double sx, sy;
    if (!gdk_event_get_position(ev, &sx, &sy))
        return false;
    GtkNative *native = gtk_widget_get_native(w->area);
    if (!native)
        return false;
    double nx, ny;
    gtk_native_get_surface_transform(native, &nx, &ny);
    graphene_point_t in = GRAPHENE_POINT_INIT((float)(sx - nx), (float)(sy - ny)), out;
    if (!gtk_widget_compute_point(GTK_WIDGET(native), w->area, &in, &out))
        return false;
    *x = out.x;
    *y = out.y;
    return true;
}

static gboolean on_pointer(GtkEventControllerLegacy *c, GdkEvent *ev, gpointer data)
{
    AuGtkWin *w = data;
    (void)c;
    GdkEventType t = gdk_event_get_event_type(ev);
    if (t != GDK_BUTTON_PRESS && t != GDK_BUTTON_RELEASE && t != GDK_MOTION_NOTIFY)
        return FALSE;
    if (w->freed || au_gtk.modal)
        return t != GDK_MOTION_NOTIFY;
    double x, y;
    if (!page_point(w, ev, &x, &y))
        return FALSE;
    w->last_x = x;
    w->last_y = y;
    uint32_t mods = au_gtk_mods(gdk_event_get_modifier_state(ev));
    if (t == GDK_MOTION_NOTIFY) {
        send_mouse(w, AU_EVENT_MOUSE_MOVE, x, y, w->pressed ? 0 : -1, mods);
        return FALSE;
    }
    guint button = gdk_button_event_get_button(ev);
    if (t == GDK_BUTTON_PRESS) {
        gtk_widget_grab_focus(w->area);
        switch (button) {
        case GDK_BUTTON_SECONDARY:
            au_gtk_context_menu(w, x, y);
            return TRUE;
        case 8: /* the side buttons: Back and Forward */
            au_bw_back(w->bw);
            au_gtk_kick();
            return TRUE;
        case 9:
            au_bw_forward(w->bw);
            au_gtk_kick();
            return TRUE;
        case GDK_BUTTON_MIDDLE:
            mods |= AU_MOD_COMMAND; /* a link opens in a new tab */
            break;
        case GDK_BUTTON_PRIMARY: break;
        default: return FALSE;
        }
        click_count(w, x, y, (int)button);
        w->pressed = true;
        w->click_mods = mods;
        send_mouse(w, AU_EVENT_MOUSE_DOWN, x, y, 0, mods);
        return TRUE;
    }
    if (button != GDK_BUTTON_PRIMARY && button != GDK_BUTTON_MIDDLE)
        return button == GDK_BUTTON_SECONDARY || button == 8 || button == 9;
    if (!w->pressed)
        return TRUE;
    w->pressed = false;
    send_mouse(w, AU_EVENT_MOUSE_UP, x, y, 0, w->click_mods | (mods & AU_MOD_SHIFT));
    return TRUE;
}

static void on_leave(GtkEventControllerMotion *m, gpointer data)
{
    AuGtkWin *w = data;
    (void)m;
    if (w->freed || au_gtk.modal || w->pressed)
        return;
    au_bw_mouse_leave(w->bw);
    au_gtk_kick();
}

/* A wheel step scrolls 48 pixels (three 16-pixel lines, as the arrow
 * keys move); touchpads scroll by the pixels they report. */
#define WHEEL_STEP 48

static gboolean on_scroll(GtkEventControllerScroll *s, double dx, double dy, gpointer data)
{
    AuGtkWin *w = data;
    if (w->freed || au_gtk.modal)
        return TRUE;
    double scale = gtk_event_controller_scroll_get_unit(s) == GDK_SCROLL_UNIT_WHEEL ? WHEEL_STEP : 1;
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(s));
    if ((state & GDK_SHIFT_MASK) && dx == 0) {
        dx = dy;
        dy = 0;
    }
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_SCROLL;
    e.time_ms = au_posix_ms();
    e.x = (int)w->last_x;
    e.y = (int)w->last_y;
    e.dx = (int)(dx * scale);
    e.dy = (int)(dy * scale);
    e.mods = au_gtk_mods(state) & ~(uint32_t)AU_MOD_SHIFT;
    if (!e.dx && !e.dy)
        return TRUE;
    au_bw_mouse(w->bw, &e);
    au_gtk_kick();
    return TRUE;
}

void au_gtk_input_attach(AuGtkWin *w)
{
    GtkIMContext *im = gtk_im_multicontext_new();
    gtk_im_context_set_client_widget(im, w->area);
    g_signal_connect(im, "commit", G_CALLBACK(on_commit), w);

    GtkEventController *k = gtk_event_controller_key_new();
    gtk_event_controller_key_set_im_context(GTK_EVENT_CONTROLLER_KEY(k), im);
    g_signal_connect(k, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(w->area, k);
    g_object_unref(im); /* the controller keeps it */

    GtkEventController *f = gtk_event_controller_focus_new();
    g_signal_connect(f, "enter", G_CALLBACK(on_page_focus_in), im);
    g_signal_connect(f, "leave", G_CALLBACK(on_page_focus_out), im);
    gtk_widget_add_controller(w->area, f);

    GtkEventController *p = gtk_event_controller_legacy_new();
    g_signal_connect(p, "event", G_CALLBACK(on_pointer), w);
    gtk_widget_add_controller(w->area, p);

    GtkEventController *m = gtk_event_controller_motion_new();
    g_signal_connect(m, "leave", G_CALLBACK(on_leave), w);
    gtk_widget_add_controller(w->area, m);

    GtkEventController *s =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES | GTK_EVENT_CONTROLLER_SCROLL_KINETIC);
    g_signal_connect(s, "scroll", G_CALLBACK(on_scroll), w);
    gtk_widget_add_controller(w->area, s);
}
