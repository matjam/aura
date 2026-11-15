/* The Preferences window: Appearance Manager controls in a tab control,
 * one pane per section (General, Fonts, Content, Privacy and Security,
 * Network, Downloads). Changes apply on OK; the tab position applies as
 * soon as it is clicked (Cancel puts it back). Settings whose subsystems
 * do not exist yet are stored, shown and editable; the subsystems read
 * them from AuPrefs (src/browser/prefs.h) when they arrive.
 *
 * Controls are created in code with Appearance procIDs and embedded in a
 * user pane per tab, which is shown or hidden as the tab changes. */
#include "mac.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kW = 560, kH = 330 }; /* fits a 640 by 480 screen */
enum { NPANES = 6 };
enum { M_STARTUP = 150, M_COOKIES, M_SERIF, M_SANS, M_MONO };

/* Field ids: one per control whose value is a preference or an action. */
typedef enum {
    F_NONE,
    F_HOME, F_USE_CURRENT, F_STARTUP, F_TABS_TOP, F_TABS_LEFT, F_SEARCH,
    F_SERIF, F_SANS, F_MONO, F_SIZE, F_SIZE_MONO, F_SIZE_MIN,
    F_JS, F_IMAGES, F_POPUPS,
    F_DNT, F_GPC, F_HTTPS_FIRST, F_TRACKERS, F_WARN_APPS, F_COOKIES, F_THIRD_PARTY,
    F_HISTORY_ON, F_HISTORY_DAYS, F_CLEAR_H_QUIT, F_CLEAR_CK_QUIT, F_CLEAR_CA_QUIT,
    F_CLEAR_HISTORY, F_CLEAR_COOKIES, F_CLEAR_CACHE,
    F_HTTP_HOST, F_HTTP_PORT, F_HTTPS_HOST, F_HTTPS_PORT, F_UA, F_MEM_CACHE, F_DISK_CACHE,
    F_DL_FOLDER, F_DECODE,
    F_OK, F_CANCEL, F_TAB,
    F_COUNT
} Field;

typedef struct PrefsWin {
    MacWin base;
    ControlHandle root, tab, panes[NPANES];
    ControlHandle field[F_COUNT];
    bool is_edit[F_COUNT];
    int pane;
    AuTabPosition orig_tabs;
    char **families;
    int nfamilies;
} PrefsWin;

static PrefsWin *g_prefs;

/* ---- Building ---- */

static ControlHandle make(PrefsWin *p, int pane, short proc, short x, short y, short w, short h, const char *title,
                          short value, short min, short max)
{
    Rect r;
    SetRect(&r, x, y, (short)(x + w), (short)(y + h));
    Str255 t;
    mac_pstr(title ? title : "", t);
    ControlHandle c = NewControl(p->base.win, &r, t, true, value, min, max, proc, 0);
    if (!c)
        mac_out_of_memory(512);
    if (pane >= 0)
        EmbedControl(c, p->panes[pane]);
    return c;
}

static void set_text(ControlHandle c, const char *utf8)
{
    size_t n;
    char *r = mac_utf8_to_roman(utf8 ? utf8 : "", &n);
    SetControlData(c, 0, kAuControlEditTextTextTag, (Size)n, r);
    au_free(r);
}

static char *get_text(ControlHandle c)
{
    char buf[1024];
    Size actual = 0;
    if (GetControlData(c, 0, kAuControlEditTextTextTag, sizeof buf, buf, &actual) != noErr)
        actual = 0;
    if (actual > (Size)sizeof buf)
        actual = sizeof buf;
    return mac_roman_to_utf8(buf, (size_t)actual);
}

static void label(PrefsWin *p, int pane, short x, short y, short w, const char *text)
{
    ControlHandle c = make(p, pane, kAuControlStaticTextProc, x, (short)(y + 2), w, 16, NULL, 0, 0, 0);
    size_t n;
    char *r = mac_utf8_to_roman(text, &n);
    SetControlData(c, 0, kAuControlStaticTextTextTag, (Size)n, r);
    au_free(r);
}

static void small_label(PrefsWin *p, int pane, short x, short y, short w, short h, const char *text)
{
    ControlHandle c = make(p, pane, kAuControlStaticTextProc, x, y, w, h, NULL, 0, 0, 0);
    size_t n;
    char *r = mac_utf8_to_roman(text, &n);
    SetControlData(c, 0, kAuControlStaticTextTextTag, (Size)n, r);
    au_free(r);
    AuControlFontStyleRec st;
    memset(&st, 0, sizeof st);
    st.flags = kAuControlUseFontMask;
    st.font = kAuControlFontSmallSystemFont;
    SetControlFontStyle(c, &st);
}

static void edit(PrefsWin *p, int pane, Field f, short x, short y, short w)
{
    p->field[f] = make(p, pane, kAuControlEditTextProc, (short)(x + 3), (short)(y + 3), (short)(w - 6), 16, NULL, 0,
                       0, 0);
    p->is_edit[f] = true;
}

static void check(PrefsWin *p, int pane, Field f, short x, short y, short w, const char *title)
{
    p->field[f] = make(p, pane, kAuControlCheckBoxAutoToggleProc, x, y, w, 18, title, 0, 0, 1);
}

static void radio(PrefsWin *p, int pane, Field f, short x, short y, short w, const char *title)
{
    p->field[f] = make(p, pane, kAuControlRadioButtonProc, x, y, w, 18, title, 0, 0, 1);
}

static void push(PrefsWin *p, int pane, Field f, short x, short y, short w, const char *title)
{
    p->field[f] = make(p, pane, kAuControlPushButtonProc, x, y, w, 20, title, 0, 0, 0);
}

static MenuHandle popup_menu(short id)
{
    MenuHandle m = GetMenuHandle(id);
    if (m) {
        DeleteMenu(id);
        DisposeMenu(m);
    }
    m = NewMenu(id, (ConstStr255Param) "\p");
    if (!m)
        mac_out_of_memory(256);
    return m;
}

static void popup(PrefsWin *p, int pane, Field f, short x, short y, short w, short menu_id, MenuHandle m)
{
    InsertMenu(m, -1);
    p->field[f] = make(p, pane, kAuControlPopupButtonProc, x, y, w, 20, NULL, 0, menu_id, 0);
    SetControlMaximum(p->field[f], CountMItems(m));
}

static void collect_families(PrefsWin *p)
{
    size_t n = au_fontset_count(g_fonts);
    p->families = au_alloc((n + 1) * sizeof *p->families);
    p->nfamilies = 0;
    for (size_t i = 0; i < n; i++) {
        const char *fam = au_fontset_entry(g_fonts, i, NULL, NULL);
        bool seen = false;
        for (int k = 0; k < p->nfamilies; k++)
            if (!strcmp(p->families[k], fam))
                seen = true;
        if (!seen)
            p->families[p->nfamilies++] = au_strndup(fam, strlen(fam));
    }
    /* Sort by name: an insertion sort is plenty for a few dozen. */
    for (int i = 1; i < p->nfamilies; i++) {
        char *x = p->families[i];
        int j = i - 1;
        while (j >= 0 && strcmp(p->families[j], x) > 0) {
            p->families[j + 1] = p->families[j];
            j--;
        }
        p->families[j + 1] = x;
    }
}

static MenuHandle families_menu(PrefsWin *p, short id)
{
    MenuHandle m = popup_menu(id);
    for (int i = 0; i < p->nfamilies; i++)
        mac_append_item(m, p->families[i]);
    if (p->nfamilies == 0)
        mac_append_item(m, "(no fonts)");
    return m;
}

static void build(PrefsWin *p)
{
    const short L = 30, X = 170, R = 530; /* label column, value column, right edge */
    short y;
    /* General */
    y = 52;
    label(p, 0, L, y, 130, "Home page:");
    edit(p, 0, F_HOME, X, y, R - X - 130);
    push(p, 0, F_USE_CURRENT, R - 120, y, 120, "Use Current Page");
    y += 34;
    label(p, 0, L, y, 130, "When Aura starts:");
    MenuHandle m = popup_menu(M_STARTUP);
    mac_append_item(m, "Open the home page");
    mac_append_item(m, "Open a blank page");
    mac_append_item(m, "Restore the last tabs");
    popup(p, 0, F_STARTUP, X, y, 220, M_STARTUP, m);
    y += 34;
    label(p, 0, L, y, 130, "Tabs:");
    radio(p, 0, F_TABS_TOP, X, (short)(y + 1), 160, "Along the top");
    radio(p, 0, F_TABS_LEFT, (short)(X + 170), (short)(y + 1), 180, "Down the left side");
    y += 34;
    label(p, 0, L, y, 130, "Search with:");
    edit(p, 0, F_SEARCH, X, y, R - X);
    y += 24;
    small_label(p, 0, X, y, R - X, 30,
                "Text in the address field that is not a web address is searched for with this URL; %s stands "
                "for the words.");

    /* Fonts */
    y = 52;
    label(p, 1, L, y, 130, "Serif:");
    popup(p, 1, F_SERIF, X, y, 220, M_SERIF, families_menu(p, M_SERIF));
    y += 32;
    label(p, 1, L, y, 130, "Sans-serif:");
    popup(p, 1, F_SANS, X, y, 220, M_SANS, families_menu(p, M_SANS));
    y += 32;
    label(p, 1, L, y, 130, "Monospace:");
    popup(p, 1, F_MONO, X, y, 220, M_MONO, families_menu(p, M_MONO));
    y += 40;
    label(p, 1, L, y, 130, "Default size:");
    edit(p, 1, F_SIZE, X, y, 50);
    label(p, 1, (short)(X + 56), y, 60, "pixels");
    y += 30;
    label(p, 1, L, y, 130, "Monospace size:");
    edit(p, 1, F_SIZE_MONO, X, y, 50);
    label(p, 1, (short)(X + 56), y, 60, "pixels");
    y += 30;
    label(p, 1, L, y, 130, "Minimum size:");
    edit(p, 1, F_SIZE_MIN, X, y, 50);
    label(p, 1, (short)(X + 56), y, 200, "pixels (0 for none)");

    /* Content */
    y = 52;
    check(p, 2, F_JS, L, y, 300, "Enable JavaScript");
    y += 26;
    check(p, 2, F_IMAGES, L, y, 300, "Load images");
    y += 26;
    check(p, 2, F_POPUPS, L, y, 300, "Block pop-up windows");
    y += 36;
    small_label(p, 2, L, y, R - L, 44,
                "Aura's script, image and network engines are not built yet. These settings are kept now and "
                "take effect when those parts arrive.");

    /* Privacy and Security */
    y = 48;
    check(p, 3, F_DNT, L, y, 240, "Send Do Not Track");
    check(p, 3, F_GPC, 290, y, 240, "Send Global Privacy Control");
    y += 22;
    check(p, 3, F_HTTPS_FIRST, L, y, 240, "HTTPS-first mode");
    check(p, 3, F_TRACKERS, 290, y, 240, "Block trackers and ads");
    y += 22;
    check(p, 3, F_WARN_APPS, L, y, 300, "Warn before downloading applications");
    y += 30;
    label(p, 3, L, y, 70, "Cookies:");
    m = popup_menu(M_COOKIES);
    mac_append_item(m, "Accept all");
    mac_append_item(m, "Same-site only");
    mac_append_item(m, "Block all");
    popup(p, 3, F_COOKIES, 100, y, 170, M_COOKIES, m);
    check(p, 3, F_THIRD_PARTY, 290, (short)(y + 1), 240, "Block third-party cookies");
    y += 32;
    check(p, 3, F_HISTORY_ON, L, (short)(y + 1), 160, "Remember history for");
    edit(p, 3, F_HISTORY_DAYS, 192, y, 50);
    label(p, 3, 248, y, 60, "days");
    y += 30;
    label(p, 3, L, y, 120, "Clear on quit:");
    check(p, 3, F_CLEAR_H_QUIT, 140, (short)(y + 1), 90, "History");
    check(p, 3, F_CLEAR_CK_QUIT, 240, (short)(y + 1), 90, "Cookies");
    check(p, 3, F_CLEAR_CA_QUIT, 340, (short)(y + 1), 90, "Cache");
    y += 34;
    push(p, 3, F_CLEAR_HISTORY, L, y, 120, "Clear History");
    push(p, 3, F_CLEAR_COOKIES, (short)(L + 130), y, 120, "Clear Cookies");
    push(p, 3, F_CLEAR_CACHE, (short)(L + 260), y, 120, "Clear Cache");

    /* Network */
    y = 52;
    label(p, 4, L, y, 130, "HTTP proxy:");
    edit(p, 4, F_HTTP_HOST, X, y, 240);
    label(p, 4, (short)(X + 248), y, 40, "Port:");
    edit(p, 4, F_HTTP_PORT, (short)(X + 290), y, 64);
    y += 30;
    label(p, 4, L, y, 130, "HTTPS proxy:");
    edit(p, 4, F_HTTPS_HOST, X, y, 240);
    label(p, 4, (short)(X + 248), y, 40, "Port:");
    edit(p, 4, F_HTTPS_PORT, (short)(X + 290), y, 64);
    y += 25;
    small_label(p, 4, X, y, R - X, 16, "Leave the host empty to connect directly.");
    y += 23;
    label(p, 4, L, y, 130, "User agent:");
    edit(p, 4, F_UA, X, y, R - X);
    y += 38;
    label(p, 4, L, y, 130, "Memory cache:");
    edit(p, 4, F_MEM_CACHE, X, y, 70);
    label(p, 4, (short)(X + 76), y, 40, "KB");
    y += 30;
    label(p, 4, L, y, 130, "Disk cache:");
    edit(p, 4, F_DISK_CACHE, X, y, 70);
    label(p, 4, (short)(X + 76), y, 40, "KB");

    /* Downloads */
    y = 52;
    label(p, 5, L, y, 130, "Save files in:");
    edit(p, 5, F_DL_FOLDER, X, y, R - X);
    y += 24;
    small_label(p, 5, X, y, R - X, 30, "A folder path such as Macintosh HD:Downloads. Leave it empty for the desktop.");
    y += 40;
    check(p, 5, F_DECODE, L, y, 400, "Decode MacBinary and BinHex files after downloading");
    y += 30;
    small_label(p, 5, L, y, R - L, 30,
                "Warning before downloading applications is under Privacy and Security.");
}

/* ---- Values ---- */

static void set_num(ControlHandle c, int v)
{
    char b[24];
    snprintf(b, sizeof b, "%d", v);
    set_text(c, b);
}

static int get_num(ControlHandle c, int fallback)
{
    char *s = get_text(c);
    char *end;
    long v = strtol(s, &end, 10);
    bool ok = *s && !*end;
    au_free(s);
    return ok ? (int)v : fallback;
}

static void select_family(PrefsWin *p, Field f, const char *fam)
{
    for (int i = 0; i < p->nfamilies; i++)
        if (au_ieq(p->families[i], strlen(p->families[i]), fam, strlen(fam)))
            SetControlValue(p->field[f], (short)(i + 1));
}

static void load(PrefsWin *p, const AuPrefs *pr)
{
    set_text(p->field[F_HOME], pr->home_page);
    SetControlValue(p->field[F_STARTUP], (short)(pr->startup + 1));
    SetControlValue(p->field[F_TABS_TOP], pr->tab_position == AU_TABS_TOP);
    SetControlValue(p->field[F_TABS_LEFT], pr->tab_position == AU_TABS_LEFT);
    set_text(p->field[F_SEARCH], pr->search_template);
    select_family(p, F_SERIF, pr->font_serif);
    select_family(p, F_SANS, pr->font_sans);
    select_family(p, F_MONO, pr->font_mono);
    set_num(p->field[F_SIZE], pr->font_size);
    set_num(p->field[F_SIZE_MONO], pr->font_size_mono);
    set_num(p->field[F_SIZE_MIN], pr->font_size_min);
    SetControlValue(p->field[F_JS], pr->javascript);
    SetControlValue(p->field[F_IMAGES], pr->load_images);
    SetControlValue(p->field[F_POPUPS], pr->block_popups);
    SetControlValue(p->field[F_DNT], pr->send_dnt);
    SetControlValue(p->field[F_GPC], pr->send_gpc);
    SetControlValue(p->field[F_HTTPS_FIRST], pr->https_first);
    SetControlValue(p->field[F_TRACKERS], pr->block_trackers);
    SetControlValue(p->field[F_WARN_APPS], pr->warn_app_downloads);
    SetControlValue(p->field[F_COOKIES], (short)(pr->cookies + 1));
    SetControlValue(p->field[F_THIRD_PARTY], pr->block_third_party_cookies);
    SetControlValue(p->field[F_HISTORY_ON], pr->history_enabled);
    set_num(p->field[F_HISTORY_DAYS], pr->history_days);
    SetControlValue(p->field[F_CLEAR_H_QUIT], pr->clear_history_on_quit);
    SetControlValue(p->field[F_CLEAR_CK_QUIT], pr->clear_cookies_on_quit);
    SetControlValue(p->field[F_CLEAR_CA_QUIT], pr->clear_cache_on_quit);
    set_text(p->field[F_HTTP_HOST], pr->http_proxy_host);
    set_num(p->field[F_HTTP_PORT], pr->http_proxy_port);
    set_text(p->field[F_HTTPS_HOST], pr->https_proxy_host);
    set_num(p->field[F_HTTPS_PORT], pr->https_proxy_port);
    set_text(p->field[F_UA], pr->user_agent);
    set_num(p->field[F_MEM_CACHE], pr->memory_cache_kb);
    set_num(p->field[F_DISK_CACHE], pr->disk_cache_kb);
    set_text(p->field[F_DL_FOLDER], pr->download_folder);
    SetControlValue(p->field[F_DECODE], pr->decode_downloads);
}

static void store_str(char **dst, ControlHandle c)
{
    char *s = get_text(c);
    au_prefs_set_str(dst, s);
    au_free(s);
}

static void store_family(PrefsWin *p, char **dst, Field f)
{
    int i = GetControlValue(p->field[f]) - 1;
    if (i >= 0 && i < p->nfamilies)
        au_prefs_set_str(dst, p->families[i]);
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void store(PrefsWin *p, AuPrefs *pr)
{
    store_str(&pr->home_page, p->field[F_HOME]);
    pr->startup = (AuStartup)clampi(GetControlValue(p->field[F_STARTUP]) - 1, 0, 2);
    pr->tab_position = GetControlValue(p->field[F_TABS_LEFT]) ? AU_TABS_LEFT : AU_TABS_TOP;
    store_str(&pr->search_template, p->field[F_SEARCH]);
    store_family(p, &pr->font_serif, F_SERIF);
    store_family(p, &pr->font_sans, F_SANS);
    store_family(p, &pr->font_mono, F_MONO);
    pr->font_size = clampi(get_num(p->field[F_SIZE], pr->font_size), 6, 72);
    pr->font_size_mono = clampi(get_num(p->field[F_SIZE_MONO], pr->font_size_mono), 6, 72);
    pr->font_size_min = clampi(get_num(p->field[F_SIZE_MIN], pr->font_size_min), 0, 36);
    pr->javascript = GetControlValue(p->field[F_JS]) != 0;
    pr->load_images = GetControlValue(p->field[F_IMAGES]) != 0;
    pr->block_popups = GetControlValue(p->field[F_POPUPS]) != 0;
    pr->send_dnt = GetControlValue(p->field[F_DNT]) != 0;
    pr->send_gpc = GetControlValue(p->field[F_GPC]) != 0;
    pr->https_first = GetControlValue(p->field[F_HTTPS_FIRST]) != 0;
    pr->block_trackers = GetControlValue(p->field[F_TRACKERS]) != 0;
    pr->warn_app_downloads = GetControlValue(p->field[F_WARN_APPS]) != 0;
    pr->cookies = (AuCookiePolicy)clampi(GetControlValue(p->field[F_COOKIES]) - 1, 0, 2);
    pr->block_third_party_cookies = GetControlValue(p->field[F_THIRD_PARTY]) != 0;
    pr->history_enabled = GetControlValue(p->field[F_HISTORY_ON]) != 0;
    pr->history_days = clampi(get_num(p->field[F_HISTORY_DAYS], pr->history_days), 0, 3650);
    pr->clear_history_on_quit = GetControlValue(p->field[F_CLEAR_H_QUIT]) != 0;
    pr->clear_cookies_on_quit = GetControlValue(p->field[F_CLEAR_CK_QUIT]) != 0;
    pr->clear_cache_on_quit = GetControlValue(p->field[F_CLEAR_CA_QUIT]) != 0;
    store_str(&pr->http_proxy_host, p->field[F_HTTP_HOST]);
    pr->http_proxy_port = clampi(get_num(p->field[F_HTTP_PORT], pr->http_proxy_port), 0, 65535);
    store_str(&pr->https_proxy_host, p->field[F_HTTPS_HOST]);
    pr->https_proxy_port = clampi(get_num(p->field[F_HTTPS_PORT], pr->https_proxy_port), 0, 65535);
    store_str(&pr->user_agent, p->field[F_UA]);
    pr->memory_cache_kb = clampi(get_num(p->field[F_MEM_CACHE], pr->memory_cache_kb), 0, 1 << 20);
    pr->disk_cache_kb = clampi(get_num(p->field[F_DISK_CACHE], pr->disk_cache_kb), 0, 1 << 22);
    store_str(&pr->download_folder, p->field[F_DL_FOLDER]);
    pr->decode_downloads = GetControlValue(p->field[F_DECODE]) != 0;
}

/* ---- Window ---- */

static void show_pane(PrefsWin *p, int pane)
{
    p->pane = pane;
    for (int i = 0; i < NPANES; i++)
        if (i != pane)
            SetControlVisibility(p->panes[i], false, true);
    SetControlVisibility(p->panes[pane], true, true);
    /* Focus the first edit field of the pane, if any. */
    SetKeyboardFocus(p->base.win, p->root, kAuControlFocusNoPart);
}

static void close_prefs(PrefsWin *p, bool ok)
{
    AuPrefs *pr = &g_browser->prefs;
    if (ok) {
        store(p, pr);
        au_browser_prefs_changed(g_browser);
    } else if (pr->tab_position != p->orig_tabs) {
        pr->tab_position = p->orig_tabs;
        au_browser_prefs_changed(g_browser);
    }
    p->base.close(&p->base);
}

static void pw_close(MacWin *mw)
{
    PrefsWin *p = (PrefsWin *)mw;
    DisposeWindow(mw->win);
    static const short menus[] = {M_STARTUP, M_COOKIES, M_SERIF, M_SANS, M_MONO};
    for (size_t i = 0; i < sizeof menus / sizeof menus[0]; i++) {
        MenuHandle m = GetMenuHandle(menus[i]);
        if (m) {
            DeleteMenu(menus[i]);
            DisposeMenu(m);
        }
    }
    for (int i = 0; i < p->nfamilies; i++)
        au_free(p->families[i]);
    au_free(p->families);
    au_free(p);
    g_prefs = NULL;
    mac_menus_dirty();
}

static void pw_update(MacWin *mw)
{
    SetPort((GrafPtr)mw->win);
    BeginUpdate(mw->win);
    EraseRect(&mw->win->portRect);
    DrawControls(mw->win);
    EndUpdate(mw->win);
}

static void pw_activate(MacWin *mw, bool on)
{
    PrefsWin *p = (PrefsWin *)mw;
    SetPort((GrafPtr)mw->win);
    if (on)
        ActivateControl(p->root);
    else
        DeactivateControl(p->root);
}

static Field field_of(PrefsWin *p, ControlHandle c)
{
    for (int i = 1; i < F_COUNT; i++)
        if (p->field[i] == c)
            return (Field)i;
    return F_NONE;
}

static void pw_click(MacWin *mw, EventRecord *ev)
{
    PrefsWin *p = (PrefsWin *)mw;
    SetPort((GrafPtr)mw->win);
    Point pt = ev->where;
    GlobalToLocal(&pt);
    SInt16 part = 0;
    ControlHandle c = FindControlUnderMouse(pt, mw->win, &part);
    if (!c)
        return;
    Field f = field_of(p, c);
    if (f != F_NONE && p->is_edit[f])
        SetKeyboardFocus(mw->win, c, kAuControlEditTextPart);
    if (!HandleControlClick(c, pt, ev->modifiers, (ControlActionUPP)-1))
        return;
    AuPrefs *pr = &g_browser->prefs;
    switch (f) {
    case F_TAB: {
        int v = GetControlValue(p->tab) - 1;
        if (v >= 0 && v < NPANES && v != p->pane)
            show_pane(p, v);
        break;
    }
    case F_TABS_TOP:
    case F_TABS_LEFT:
        SetControlValue(p->field[F_TABS_TOP], f == F_TABS_TOP);
        SetControlValue(p->field[F_TABS_LEFT], f == F_TABS_LEFT);
        pr->tab_position = f == F_TABS_LEFT ? AU_TABS_LEFT : AU_TABS_TOP;
        au_browser_prefs_changed(g_browser);
        browserwin_flush_all();
        break;
    case F_USE_CURRENT: {
        BrowserWin *b = browserwin_front();
        AuTab *t = b ? au_bw_active_tab(browserwin_model(b)) : NULL;
        if (t)
            set_text(p->field[F_HOME], au_page_url(t->page));
        Draw1Control(p->field[F_HOME]);
        break;
    }
    case F_CLEAR_HISTORY:
        au_history_clear(&g_browser->history);
        au_browser_history_changed(g_browser);
        break;
    case F_CLEAR_COOKIES:
    case F_CLEAR_CACHE:
        /* No cookie store or cache exists yet: nothing to clear. */
        break;
    case F_OK: close_prefs(p, true); break;
    case F_CANCEL: close_prefs(p, false); break;
    default: break;
    }
}

static bool pw_key(MacWin *mw, EventRecord *ev)
{
    PrefsWin *p = (PrefsWin *)mw;
    unsigned char ch = (unsigned char)(ev->message & charCodeMask);
    SetPort((GrafPtr)mw->win);
    if (ch == 0x0D || ch == 0x03) {
        close_prefs(p, true);
        return true;
    }
    if (ch == 0x1B) {
        close_prefs(p, false);
        return true;
    }
    if (ch == 0x09) {
        AdvanceKeyboardFocus(mw->win);
        return true;
    }
    ControlHandle focus = NULL;
    if (GetKeyboardFocus(mw->win, &focus) == noErr && focus)
        HandleControlKey(focus, (SInt16)((ev->message >> 8) & 0xFF), (SInt16)ch, ev->modifiers);
    return true;
}

static bool pw_edit(MacWin *mw, int item)
{
    ControlHandle focus = NULL;
    if (GetKeyboardFocus(mw->win, &focus) != noErr || !focus)
        return false;
    TEHandle te = NULL;
    Size actual;
    if (GetControlData(focus, 0, kAuControlEditTextTEHandleTag, sizeof te, (Ptr)&te, &actual) != noErr || !te)
        return false;
    if (item == 0)
        return true;
    SetPort((GrafPtr)mw->win);
    switch (item) {
    case 3: TECut(te); ZeroScrap(); TEToScrap(); break;
    case 4: TECopy(te); ZeroScrap(); TEToScrap(); break;
    case 5: TEFromScrap(); TEPaste(te); break;
    case 6: TEDelete(te); break;
    case 7: TESetSelect(0, 32767, te); break;
    default: return false;
    }
    return true;
}

static void pw_idle(MacWin *mw)
{
    if (mw->win == FrontWindow() && g_in_front) {
        SetPort((GrafPtr)mw->win);
        IdleControls(mw->win);
    }
}

void prefswin_show(void)
{
    if (g_prefs) {
        SelectWindow(g_prefs->base.win);
        return;
    }
    PrefsWin *p = au_calloc(sizeof *p);
    p->base.kind = WIN_PREFS;
    p->base.update = pw_update;
    p->base.activate = pw_activate;
    p->base.click = pw_click;
    p->base.key = pw_key;
    p->base.idle = pw_idle;
    p->base.close = pw_close;
    p->base.edit = pw_edit;
    Rect screen = qd.screenBits.bounds, wr;
    int x = (screen.right - kW) / 2, y = 60;
    if (x < 4)
        x = 4;
    SetRect(&wr, (short)x, (short)y, (short)(x + kW), (short)(y + kH));
    WindowPtr win = NewCWindow(NULL, &wr, (ConstStr255Param) "\pPreferences", false, kMovableModalProc,
                               (WindowPtr)-1, false, (long)p);
    if (!win)
        mac_out_of_memory(sizeof(WindowRecord));
    p->base.win = win;
    g_prefs = p;
    SetPort((GrafPtr)win);
    SetThemeWindowBackground(win, kAuThemeBrushDialogBackgroundActive, false);
    if (CreateRootControl(win, &p->root) != noErr) {
        DisposeWindow(win);
        au_free(p);
        g_prefs = NULL;
        return;
    }
    p->orig_tabs = g_browser->prefs.tab_position;
    collect_families(p);
    p->tab = make(p, -1, kAuControlTabSmallProc, 10, 10, kW - 20, kH - 60, NULL, 128, 0, 0);
    p->field[F_TAB] = p->tab;
    for (int i = 0; i < NPANES; i++) {
        Rect r;
        SetRect(&r, 14, 40, kW - 14, kH - 54);
        p->panes[i] = NewControl(win, &r, (ConstStr255Param) "\p", true, kAuControlSupportsEmbedding, 0, 0,
                                 kAuControlUserPaneProc, 0);
        if (!p->panes[i])
            mac_out_of_memory(256);
        EmbedControl(p->panes[i], p->tab);
    }
    build(p);
    push(p, -1, F_CANCEL, kW - 190, kH - 36, 80, "Cancel");
    push(p, -1, F_OK, kW - 98, kH - 36, 80, "OK");
    Boolean yes = true;
    SetControlData(p->field[F_OK], 0, kAuControlPushButtonDefaultTag, sizeof yes, (Ptr)&yes);
    load(p, &g_browser->prefs);
    show_pane(p, 0);
    ShowWindow(win);
    SelectWindow(win);
    mac_menus_dirty();
}
