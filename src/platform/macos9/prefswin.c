/* The Preferences window: Appearance Manager controls in a tab control,
 * one pane per section (General, Fonts, Content, Privacy and Security,
 * Network, Downloads, Blocking), built from the preference descriptions
 * and run through an editing session (src/browser/prefs_ui.h).
 *
 * This file holds only what is the Toolbox's: the layout table below
 * (where each item's controls go), making the controls, the keyboard
 * focus, and the parts of the buttons that need the OS (Internet
 * Config, the cookie store and cache, the Cookies window). Every value,
 * check, choice list, note and enabled state comes from the session.
 * prefs_blocking.c makes the Blocking pane's list rows.
 *
 * Changes apply on OK; the tab position, the appearance and the
 * blocking switch and lists apply as soon as they are clicked (Cancel
 * puts them back), and
 * the network settings reach the network stack through
 * mac_net_prefs_changed.
 *
 * Controls are created in code with Appearance procIDs and embedded in a
 * user pane per tab, which is shown or hidden as the tab changes. */
#include "mac.h"
#include "blocking_mac.h"
#include "browser/prefs_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kW = 616, kH = 392 }; /* fits a 640 by 480 screen; seven tabs need the width */
enum { NPANES = AU_PREF_SECTION_COUNT };

/* ---- Layout ----
 * One row per item, in window coordinates. cap: the caption's x, y and
 * width (cw 0: no caption). The control at x, y, w (h for notes). x2,
 * w2: a radio group's second button. ux, uw: the unit after a number.
 * sx, sy, sw: the status line. menu: a pop-up's menu ID. */
enum {
    H_SMALL = 1,       /* the status line is in the small system font */
    H_TWO_LINES = 2,   /* the caption breaks at its last space onto a second line */
    H_AFTER_LIST = 4,  /* y and cy are below the blocking list rows */
    H_STATUS_LIST = 8  /* sy is below the blocking list rows */
};

typedef struct Hint {
    AuPrefId id;
    short cx, cy, cw;
    short x, y, w, h;
    short x2, w2;
    short ux, uw;
    short sx, sy, sw;
    short menu;
    unsigned char flags;
} Hint;

enum { L = 30, X = 170, R = 530 }; /* label column, value column, right edge */

/* 155 was the Blocking pane's update menu before the descriptions. */
static const Hint hints[] = {
    /* id                              cx  cy  cw    x    y    w   h   x2   w2   ux   uw   sx   sy   sw  menu flags */
    {AU_PREF_HOME_PAGE,                 L, 52, 130,   X,  52, R - X - 130, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_USE_CURRENT_PAGE,          0, 0, 0,  R - 120, 52, 120, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_STARTUP,                   L, 86, 130,   X,  86, 220, 0, 0, 0, 0, 0, 0, 0, 0, 150, 0},
    {AU_PREF_TAB_POSITION,              L, 120, 130,  X, 121, 160, 0, X + 170, 180, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_APPEARANCE,                L, 154, 130,  X, 154, 160, 0, 0, 0, 0, 0, 0, 0, 0, 158, 0},
    {AU_PREF_SEARCH,                    L, 188, 130,  X, 188, R - X, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_SEARCH_NOTE,               0, 0, 0,      X, 212, R - X, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_EXTERNAL_NEW_WINDOW,       L, 248, 130,  X, 249, R - X, 0, 0, 0, 0, 0, 0, 0, 0, 0, H_TWO_LINES},
    {AU_PREF_DEFAULT_BROWSER,           0, 0, 0,      X, 282, 220, 0, 0, 0, 0, 0, X + 230, 285, R - X - 230, 0, 0},
    {AU_PREF_FONT_SERIF,                L, 52, 130,   X,  52, 220, 0, 0, 0, 0, 0, 0, 0, 0, 152, 0},
    {AU_PREF_FONT_SANS,                 L, 84, 130,   X,  84, 220, 0, 0, 0, 0, 0, 0, 0, 0, 153, 0},
    {AU_PREF_FONT_MONO,                 L, 116, 130,  X, 116, 220, 0, 0, 0, 0, 0, 0, 0, 0, 154, 0},
    {AU_PREF_FONT_SIZE,                 L, 156, 130,  X, 156, 50, 0, 0, 0, X + 56, 60, 0, 0, 0, 0, 0},
    {AU_PREF_FONT_SIZE_MONO,            L, 186, 130,  X, 186, 50, 0, 0, 0, X + 56, 60, 0, 0, 0, 0, 0},
    {AU_PREF_FONT_SIZE_MIN,             L, 216, 130,  X, 216, 50, 0, 0, 0, X + 56, 200, 0, 0, 0, 0, 0},
    {AU_PREF_JAVASCRIPT,                0, 0, 0,      L,  52, 300, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_LOAD_IMAGES,               0, 0, 0,      L,  78, 300, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_BLOCK_POPUPS,              0, 0, 0,      L, 104, 300, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_SCRIPT_MEMORY_MODE,        L, 144, 190, 222, 144, 112, 0, 0, 0, 0, 0, 0, 0, 0, 156, 0},
    {AU_PREF_SCRIPT_MEMORY_MB,          0, 0, 0,    340, 144, 46, 0, 0, 0, 390, 26, 420, 148, 186, 0, H_SMALL},
    {AU_PREF_WORKER_MEMORY_MODE,        L, 174, 190, 222, 174, 112, 0, 0, 0, 0, 0, 0, 0, 0, 157, 0},
    {AU_PREF_WORKER_MEMORY_MB,          0, 0, 0,    340, 174, 46, 0, 0, 0, 390, 26, 420, 178, 186, 0, H_SMALL},
    {AU_PREF_IMAGE_MEMORY_MODE,         L, 204, 190, 222, 204, 112, 0, 0, 0, 0, 0, 0, 0, 0, 159, 0},
    {AU_PREF_IMAGE_MEMORY_MB,           0, 0, 0,    340, 204, 46, 0, 0, 0, 390, 26, 420, 208, 186, 0, H_SMALL},
    {AU_PREF_IMAGE_COLORS,              L, 234, 190, 222, 234, 190, 0, 0, 0, 0, 0, 0, 0, 0, 160, 0},
    {AU_PREF_SCRIPT_MEMORY_NOTE,        0, 0, 0,      L, 264, R - L, 70, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_SEND_DNT,                  0, 0, 0,      L,  48, 240, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_SEND_GPC,                  0, 0, 0,    290,  48, 240, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_HTTPS_FIRST,               0, 0, 0,      L,  70, 240, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_LEGACY_TLS,                0, 0, 0,    290,  70, 300, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_WARN_APP_DOWNLOADS,        0, 0, 0,      L,  92, 300, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_COOKIES,                   L, 122, 70, 100, 122, 170, 0, 0, 0, 0, 0, 0, 0, 0, 151, 0},
    {AU_PREF_BLOCK_THIRD_PARTY_COOKIES, 0, 0, 0,    290, 123, 240, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_HISTORY_ENABLED,           0, 0, 0,      L, 155, 160, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_HISTORY_DAYS,              0, 0, 0,    192, 154, 50, 0, 0, 0, 248, 60, 0, 0, 0, 0, 0},
    {AU_PREF_CLEAR_HISTORY_ON_QUIT,     L, 184, 120, 140, 185, 90, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_CLEAR_COOKIES_ON_QUIT,     0, 0, 0,    240, 185, 90, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_CLEAR_CACHE_ON_QUIT,       0, 0, 0,    340, 185, 90, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_CLEAR_HISTORY,             0, 0, 0,      L, 218, 120, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_CLEAR_COOKIES,             0, 0, 0, L + 130, 218, 120, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_CLEAR_CACHE,               0, 0, 0, L + 260, 218, 120, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_SHOW_COOKIES,              0, 0, 0, L + 390, 218, 110, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_HTTP_PROXY_HOST,           L, 52, 130,   X,  52, 240, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_HTTP_PROXY_PORT,     X + 248, 52, 40, X + 290, 52, 64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_HTTPS_PROXY_HOST,          L, 82, 130,   X,  82, 240, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_HTTPS_PROXY_PORT,    X + 248, 82, 40, X + 290, 82, 64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_PROXY_NOTE,                0, 0, 0,      X, 107, R - X, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_USER_AGENT,                L, 130, 130,  X, 130, R - X, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_MEMORY_CACHE,              L, 168, 130,  X, 168, 70, 0, 0, 0, X + 76, 40, 0, 0, 0, 0, 0},
    {AU_PREF_DISK_CACHE,                L, 198, 130,  X, 198, 70, 0, 0, 0, X + 76, 40, 0, 0, 0, 0, 0},
    {AU_PREF_UNLOAD_TABS,               0, 0, 0,      L, 232, 400, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_DOWNLOAD_FOLDER,           L, 52, 130,   X,  52, R - X, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_DOWNLOAD_FOLDER_NOTE,      0, 0, 0,      X,  76, R - X, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_DECODE_DOWNLOADS,          0, 0, 0,      L, 116, 400, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_DOWNLOADS_NOTE,            0, 0, 0,      L, 146, R - L, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_BLOCK_TRACKERS,            0, 0, 0,      L,  46, 400, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_BLOCK_LISTS,               0, 0, 0,      L,  68, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_BLOCK_UPDATE,              L, 0, 110,  144,   0, 130, 0, 0, 0, 0, 0, 0, 0, 0, 155, H_AFTER_LIST},
    {AU_PREF_BLOCK_UPDATE_NOW,          0, 0, 0,    290,   0, 110, 0, 0, 0, 0, 0, 0, 0, 0, 0, H_AFTER_LIST},
    {AU_PREF_BLOCK_CUSTOM,              L, 230, 110, 144, 230, R - 144, 0, 0, 0, 0, 0, 144, 50, 390, 0,
     H_SMALL | H_STATUS_LIST},
    {AU_PREF_BLOCK_ALLOW,               L, 270, 110, 144, 270, R - 144, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {AU_PREF_BLOCK_DENY,                L, 296, 110, 144, 296, R - 144, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

static const Hint *hint_of(AuPrefId id)
{
    for (size_t i = 0; i < sizeof hints / sizeof hints[0]; i++)
        if (hints[i].id == id)
            return &hints[i];
    return NULL;
}

typedef struct PrefsWin {
    MacWin base;
    AuPrefsEdit *edit;
    ControlHandle root, tab, panes[NPANES], ok, cancel;
    ControlHandle ctl[AU_PREF_COUNT];    /* the item's control (a radio group's first button) */
    ControlHandle ctl2[AU_PREF_COUNT];   /* a radio group's second button */
    ControlHandle status[AU_PREF_COUNT]; /* the status line */
    bool disabled[AU_PREF_COUNT];        /* as shown */
    char shown[AU_PREF_COUNT][256];      /* the status text on screen */
    int pane;
    AuPrefId focus;      /* the edit field with the keyboard focus, or AU_PREF_NONE */
    AuPrefId last_click; /* for double-clicks */
    unsigned long last_click_ticks;
} PrefsWin;

static PrefsWin *g_prefs;

static AuPrefKind kind_of(AuPrefId id)
{
    return au_pref_desc(id)->kind;
}

static bool is_edit(AuPrefId id)
{
    AuPrefKind k = kind_of(id);
    return k == AU_PREF_NUMBER || k == AU_PREF_TEXT || k == AU_PREF_FOLDER;
}

static int pane_of(AuPrefId id)
{
    return (int)au_pref_desc(id)->section;
}

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

static void set_static(ControlHandle c, const char *utf8)
{
    size_t n;
    char *r = mac_utf8_to_roman(utf8, &n);
    SetControlData(c, 0, kAuControlStaticTextTextTag, (Size)n, r);
    au_free(r);
}

static void use_small_font(ControlHandle c)
{
    AuControlFontStyleRec st;
    memset(&st, 0, sizeof st);
    st.flags = kAuControlUseFontMask;
    st.font = kAuControlFontSmallSystemFont;
    SetControlFontStyle(c, &st);
}

static void label(PrefsWin *p, int pane, short x, short y, short w, const char *text)
{
    ControlHandle c = make(p, pane, kAuControlStaticTextProc, x, (short)(y + 2), w, 16, NULL, 0, 0, 0);
    set_static(c, text);
}

static void small_label(PrefsWin *p, int pane, short x, short y, short w, short h, const char *text)
{
    ControlHandle c = make(p, pane, kAuControlStaticTextProc, x, y, w, h, NULL, 0, 0, 0);
    set_static(c, text);
    use_small_font(c);
}

/* The caption, on one line or broken at its last space onto two. */
static void caption(PrefsWin *p, int pane, const Hint *h, short cy, const char *text)
{
    const char *sp = (h->flags & H_TWO_LINES) ? strrchr(text, ' ') : NULL;
    if (!sp) {
        label(p, pane, h->cx, cy, h->cw, text);
        return;
    }
    char *first = au_strndup(text, (size_t)(sp - text));
    label(p, pane, h->cx, cy, h->cw, first);
    label(p, pane, h->cx, (short)(cy + 14), h->cw, sp + 1);
    au_free(first);
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

/* One item's controls. list_end: the y below the blocking list rows. */
static void build_item(PrefsWin *p, AuPrefId id, short *list_end)
{
    const AuPrefDesc *d = au_pref_desc(id);
    const Hint *h = hint_of(id);
    if (!h)
        return;
    int pane = (int)d->section;
    short y = h->y, cy = h->cy, sy = h->sy;
    if (h->flags & H_AFTER_LIST) {
        y = (short)(y + *list_end);
        cy = (short)(cy + *list_end);
    }
    if (h->flags & H_STATUS_LIST)
        sy = (short)(sy + *list_end);
    if (d->caption && h->cw)
        caption(p, pane, h, cy, d->caption);
    switch (d->kind) {
    case AU_PREF_CHECK:
        p->ctl[id] = make(p, pane, kAuControlCheckBoxAutoToggleProc, h->x, y, h->w, 18, d->title, 0, 0, 1);
        break;
    case AU_PREF_RADIO:
        p->ctl[id] = make(p, pane, kAuControlRadioButtonProc, h->x, y, h->w, 18, d->choices[0], 0, 0, 1);
        p->ctl2[id] = make(p, pane, kAuControlRadioButtonProc, h->x2, y, h->w2, 18, d->choices[1], 0, 0, 1);
        break;
    case AU_PREF_NUMBER:
    case AU_PREF_TEXT:
    case AU_PREF_FOLDER:
        p->ctl[id] = make(p, pane, kAuControlEditTextProc, (short)(h->x + 3), (short)(y + 3), (short)(h->w - 6), 16,
                          NULL, 0, 0, 0);
        break;
    case AU_PREF_CHOICE:
    case AU_PREF_FONT: {
        MenuHandle m = popup_menu(h->menu);
        size_t n = au_prefs_edit_choice_count(p->edit, id);
        for (size_t i = 0; i < n; i++)
            mac_append_item(m, au_prefs_edit_choice(p->edit, id, i));
        if (n == 0 && d->empty)
            mac_append_item(m, d->empty);
        InsertMenu(m, -1);
        p->ctl[id] = make(p, pane, kAuControlPopupButtonProc, h->x, y, h->w, 20, NULL, 0, h->menu, 0);
        SetControlMaximum(p->ctl[id], CountMItems(m));
        break;
    }
    case AU_PREF_ACTION:
        p->ctl[id] = make(p, pane, kAuControlPushButtonProc, h->x, y, h->w, 20, d->title, 0, 0, 0);
        break;
    case AU_PREF_NOTE: {
        AuBuf b;
        au_buf_init(&b);
        au_prefs_edit_text(p->edit, id, &b);
        small_label(p, pane, h->x, y, h->w, h->h, au_buf_cstr(&b));
        au_buf_free(&b);
        break;
    }
    case AU_PREF_LIST:
        *list_end = prefs_blocking_build(p->base.win, p->panes[pane], p->edit, y);
        break;
    }
    if (d->unit && h->uw)
        label(p, pane, h->ux, y, h->uw, d->unit);
    if ((d->flags & AU_PREF_STATUS) && h->sw) {
        p->status[id] = make(p, pane, kAuControlStaticTextProc, h->sx, sy, h->sw, (h->flags & H_SMALL) ? 14 : 16, NULL,
                             0, 0, 0);
        if (h->flags & H_SMALL)
            use_small_font(p->status[id]);
    }
}

static void build(PrefsWin *p)
{
    short list_end = 0;
    for (int i = 0; i < AU_PREF_COUNT; i++)
        build_item(p, (AuPrefId)i, &list_end);
}

/* ---- Values ---- */

static void drop_focus(PrefsWin *p);

static void load_item(PrefsWin *p, AuPrefId id)
{
    ControlHandle c = p->ctl[id];
    if (!c)
        return;
    switch (kind_of(id)) {
    case AU_PREF_CHECK:
        SetControlValue(c, au_prefs_edit_get_bool(p->edit, id));
        break;
    case AU_PREF_RADIO: {
        int v = au_prefs_edit_get_choice(p->edit, id);
        SetControlValue(c, v == 0);
        SetControlValue(p->ctl2[id], v == 1);
        break;
    }
    case AU_PREF_NUMBER:
    case AU_PREF_TEXT:
    case AU_PREF_FOLDER: {
        AuBuf b;
        au_buf_init(&b);
        au_prefs_edit_get_text(p->edit, id, &b);
        set_text(c, au_buf_cstr(&b));
        au_buf_free(&b);
        break;
    }
    case AU_PREF_CHOICE:
    case AU_PREF_FONT: {
        int v = au_prefs_edit_get_choice(p->edit, id);
        if (v >= 0)
            SetControlValue(c, (short)(v + 1));
        break;
    }
    default:
        break;
    }
}

/* Status lines and enabled states, after a change (redraw: now). */
static void refresh_states(PrefsWin *p, bool redraw)
{
    AuBuf b;
    au_buf_init(&b);
    for (int i = 0; i < AU_PREF_COUNT; i++) {
        AuPrefId id = (AuPrefId)i;
        if (p->status[id]) {
            au_buf_clear(&b);
            au_prefs_edit_text(p->edit, id, &b);
            if (strcmp(au_buf_cstr(&b), p->shown[id])) {
                snprintf(p->shown[id], sizeof p->shown[id], "%s", au_buf_cstr(&b));
                set_static(p->status[id], au_buf_cstr(&b));
                if (redraw)
                    Draw1Control(p->status[id]);
            }
        }
        bool off = !au_prefs_edit_enabled(p->edit, id);
        if (!p->ctl[id] || off == p->disabled[id])
            continue;
        p->disabled[id] = off;
        if (kind_of(id) == AU_PREF_ACTION) {
            HiliteControl(p->ctl[id], off ? 255 : 0);
        } else if (off) {
            if (p->focus == id)
                drop_focus(p);
            DeactivateControl(p->ctl[id]);
        } else {
            ActivateControl(p->ctl[id]);
        }
        if (redraw)
            Draw1Control(p->ctl[id]);
    }
    au_buf_free(&b);
}

static void load(PrefsWin *p)
{
    for (int i = 0; i < AU_PREF_COUNT; i++)
        load_item(p, (AuPrefId)i);
    refresh_states(p, false);
    prefs_blocking_load();
}

/* OK: every control's value into the session. */
static void store(PrefsWin *p)
{
    for (int i = 0; i < AU_PREF_COUNT; i++) {
        AuPrefId id = (AuPrefId)i;
        ControlHandle c = p->ctl[id];
        if (!c)
            continue;
        switch (kind_of(id)) {
        case AU_PREF_CHECK:
            au_prefs_edit_set_bool(p->edit, id, GetControlValue(c) != 0);
            break;
        case AU_PREF_RADIO:
            au_prefs_edit_set_choice(p->edit, id, GetControlValue(p->ctl2[id]) ? 1 : 0);
            break;
        case AU_PREF_CHOICE:
        case AU_PREF_FONT:
            au_prefs_edit_set_choice(p->edit, id, GetControlValue(c) - 1);
            break;
        case AU_PREF_NUMBER:
        case AU_PREF_TEXT:
        case AU_PREF_FOLDER: {
            char *s = get_text(c);
            au_prefs_edit_set_text(p->edit, id, s);
            au_free(s);
            break;
        }
        default:
            break;
        }
    }
}

/* What Internet Config says about the default browser. Reading the
 * setting changes nothing; only the button's click writes it. */
static AuDefaultBrowser default_browser_state(void)
{
    if (!mac_ic_available())
        return AU_DEFAULT_BROWSER_UNAVAILABLE;
    return mac_ic_is_default_browser() ? AU_DEFAULT_BROWSER_YES : AU_DEFAULT_BROWSER_NO;
}

/* ---- Keyboard focus ----
 * Aura tracks which edit field has the focus itself rather than leaving
 * it to AdvanceKeyboardFocus, which walks into the hidden panes: only a
 * field of the visible pane ever holds the focus (and blinks a caret).
 * Tab and Shift-Tab move between the visible pane's fields and select the
 * whole contents, as Mac dialogs do; a click places the caret; a
 * double-click selects a word, and the whole number in a number field. */

static TEHandle field_te(PrefsWin *p, AuPrefId f)
{
    TEHandle te = NULL;
    Size actual;
    if (f == AU_PREF_NONE || !p->ctl[f] ||
        GetControlData(p->ctl[f], 0, kAuControlEditTextTEHandleTag, sizeof te, (Ptr)&te, &actual) != noErr)
        return NULL;
    return te;
}

static void drop_focus(PrefsWin *p)
{
    ControlHandle cur = NULL;
    if (GetKeyboardFocus(p->base.win, &cur) == noErr && cur)
        SetKeyboardFocus(p->base.win, cur, kAuControlFocusNoPart);
    p->focus = AU_PREF_NONE;
}

static void select_all_in(PrefsWin *p, AuPrefId f)
{
    TEHandle te = field_te(p, f);
    if (te)
        TESetSelect(0, 32767, te);
}

static void focus_edit(PrefsWin *p, AuPrefId f, bool select_all)
{
    SetPort((GrafPtr)p->base.win);
    if (p->focus != f) {
        drop_focus(p);
        if (SetKeyboardFocus(p->base.win, p->ctl[f], kAuControlEditTextPart) == noErr)
            p->focus = f;
    }
    if (select_all && p->focus == f)
        select_all_in(p, f);
}

/* The next (dir 1) or previous (dir -1) edit field of the visible pane
 * after the focused one, wrapping; AU_PREF_NONE when the pane has none. */
static AuPrefId next_edit(PrefsWin *p, int dir)
{
    int start = p->focus != AU_PREF_NONE && pane_of(p->focus) == p->pane ? (int)p->focus
                                                                            : (dir > 0 ? -1 : AU_PREF_COUNT);
    for (int k = 1; k <= AU_PREF_COUNT; k++) {
        int i = ((start + dir * k) % AU_PREF_COUNT + AU_PREF_COUNT) % AU_PREF_COUNT;
        AuPrefId id = (AuPrefId)i;
        if (p->ctl[id] && is_edit(id) && pane_of(id) == p->pane && au_prefs_edit_enabled(p->edit, id))
            return id;
    }
    return AU_PREF_NONE;
}

/* Selects the word around the caret (letters, digits and dots). */
static void select_word(PrefsWin *p, AuPrefId f)
{
    TEHandle te = field_te(p, f);
    if (!te)
        return;
    SignedByte st = HGetState((Handle)te);
    HLock((Handle)te);
    CharsHandle h = TEGetText(te);
    SignedByte hs = HGetState((Handle)h);
    HLock((Handle)h);
    const unsigned char *s = (const unsigned char *)*h;
    short n = (**te).teLength, a = (**te).selStart, b = a;
#define WORDCH(c) (((c) >= '0' && (c) <= '9') || ((c) >= 'A' && (c) <= 'Z') || ((c) >= 'a' && (c) <= 'z') || \
                   (c) == '.' || (c) == '-' || (c) >= 0x80)
    while (a > 0 && WORDCH(s[a - 1]))
        a--;
    while (b < n && WORDCH(s[b]))
        b++;
#undef WORDCH
    HSetState((Handle)h, hs);
    HSetState((Handle)te, st);
    if (b > a)
        TESetSelect(a, b, te);
}

static void show_pane(PrefsWin *p, int pane)
{
    /* Hidden controls must not hold the focus or draw a caret. */
    drop_focus(p);
    p->pane = pane;
    for (int i = 0; i < NPANES; i++)
        if (i != pane)
            SetControlVisibility(p->panes[i], false, true);
    SetControlVisibility(p->panes[pane], true, true);
    /* The pane's first edit field takes the focus, its text selected. */
    AuPrefId f = next_edit(p, 1);
    if (f != AU_PREF_NONE)
        focus_edit(p, f, true);
}

/* ---- Window ---- */

static void close_prefs(PrefsWin *p, bool ok)
{
    if (ok) {
        store(p);
        au_prefs_edit_commit(p->edit, g_browser);
        mac_net_prefs_changed();
    } else if (au_prefs_edit_cancel(p->edit, g_browser) & AU_PREF_CHANGED_BLOCKING) {
        mac_blocking_prefs_changed();
    }
    p->base.close(&p->base);
}

static void pw_close(MacWin *mw)
{
    PrefsWin *p = (PrefsWin *)mw;
    prefs_blocking_close();
    DisposeWindow(mw->win);
    for (size_t i = 0; i < sizeof hints / sizeof hints[0]; i++) {
        MenuHandle m = hints[i].menu ? GetMenuHandle(hints[i].menu) : NULL;
        if (m) {
            DeleteMenu(hints[i].menu);
            DisposeMenu(m);
        }
    }
    au_prefs_edit_free(p->edit);
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

/* The item a control belongs to; second: it is a radio group's second button. */
static AuPrefId item_of(PrefsWin *p, ControlHandle c, bool *second)
{
    *second = false;
    for (int i = 0; i < AU_PREF_COUNT; i++) {
        if (p->ctl[i] == c)
            return (AuPrefId)i;
        if (p->ctl2[i] == c) {
            *second = true;
            return (AuPrefId)i;
        }
    }
    return AU_PREF_NONE;
}

/* A live item changed in the session: apply it to the browser now. */
static void apply_live(PrefsWin *p)
{
    unsigned bits = au_prefs_edit_apply_live(p->edit, g_browser);
    /* The appearance restyles every page (au_browser_prefs_changed); the
     * flush draws them now rather than at the next idle. */
    if (bits & (AU_PREF_CHANGED_TABS | AU_PREF_CHANGED_APPEARANCE))
        browserwin_flush_all();
    if (bits & AU_PREF_CHANGED_BLOCKING)
        mac_blocking_prefs_changed();
}

static void blocking_changed(PrefsWin *p)
{
    prefs_blocking_refresh(true);
    refresh_states(p, true);
}

static void run_action(PrefsWin *p, AuPrefId id)
{
    BrowserWin *front = browserwin_front();
    AuPrefId changed;
    unsigned todo = au_prefs_edit_action(p->edit, id, g_browser, front ? browserwin_model(front) : NULL, &changed);
    if (id == AU_PREF_USE_CURRENT_PAGE)
        changed = AU_PREF_HOME_PAGE; /* drawn whether or not a page was open, as before */
    if (todo & AU_PREF_DO_RELOAD)
        load_item(p, changed);
    if (changed != AU_PREF_NONE && p->ctl[changed])
        Draw1Control(p->ctl[changed]);
    if (todo & AU_PREF_DO_MAKE_DEFAULT) {
        /* The one place Aura writes Internet Config: the http, https and
         * file helpers become Aura. */
        if (!mac_ic_make_default_browser())
            SysBeep(1);
        au_prefs_edit_set_default_browser(p->edit, default_browser_state());
        refresh_states(p, true);
    }
    if (todo & AU_PREF_DO_CLEAR_COOKIES) {
        mac_net_clear_cookies();
        listwin_cookies_changed();
    }
    if (todo & AU_PREF_DO_CLEAR_CACHE)
        mac_net_clear_cache();
    if (todo & AU_PREF_DO_SHOW_COOKIES)
        listwin_show(WIN_COOKIES);
    if (todo & AU_PREF_DO_REFRESH_LISTS)
        blocking_changed(p);
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
    bool second;
    AuPrefId id = item_of(p, c, &second);
    if (id != AU_PREF_NONE && is_edit(id)) {
        if (!au_prefs_edit_enabled(p->edit, id))
            return;
        bool dbl = id == p->last_click && ev->when - p->last_click_ticks <= (unsigned long)GetDblTime();
        p->last_click = id;
        p->last_click_ticks = ev->when;
        focus_edit(p, id, false);
        HandleControlClick(c, pt, ev->modifiers, (ControlActionUPP)-1);
        if (dbl) {
            if (kind_of(id) == AU_PREF_NUMBER)
                select_all_in(p, id);
            else
                select_word(p, id);
            p->last_click = AU_PREF_NONE; /* a third click starts over */
        }
        return;
    }
    p->last_click = AU_PREF_NONE;
    if (!HandleControlClick(c, pt, ev->modifiers, (ControlActionUPP)-1))
        return;
    if (c == p->tab) {
        int v = GetControlValue(p->tab) - 1;
        if (v >= 0 && v < NPANES && v != p->pane)
            show_pane(p, v);
        return;
    }
    if (c == p->ok) {
        close_prefs(p, true);
        return;
    }
    if (c == p->cancel) {
        close_prefs(p, false);
        return;
    }
    if (id == AU_PREF_NONE) {
        if (prefs_blocking_click(c)) {
            apply_live(p);
            blocking_changed(p);
        }
        return;
    }
    const AuPrefDesc *d = au_pref_desc(id);
    switch (d->kind) {
    case AU_PREF_RADIO:
        SetControlValue(p->ctl[id], !second);
        SetControlValue(p->ctl2[id], second);
        au_prefs_edit_set_choice(p->edit, id, second ? 1 : 0);
        break;
    case AU_PREF_CHECK:
        au_prefs_edit_set_bool(p->edit, id, GetControlValue(c) != 0);
        break;
    case AU_PREF_CHOICE:
    case AU_PREF_FONT:
        au_prefs_edit_set_choice(p->edit, id, GetControlValue(c) - 1);
        break;
    case AU_PREF_ACTION:
        run_action(p, id);
        return;
    default:
        return;
    }
    if (d->flags & AU_PREF_LIVE)
        apply_live(p);
    /* Automatic script memory follows the memory free now. */
    au_prefs_edit_set_memory_free(p->edit, mac_memory_free_kb());
    refresh_states(p, true);
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
        AuPrefId f = next_edit(p, (ev->modifiers & shiftKey) ? -1 : 1);
        if (f != AU_PREF_NONE)
            focus_edit(p, f, true);
        return true;
    }
    if (p->focus != AU_PREF_NONE && pane_of(p->focus) == p->pane)
        HandleControlKey(p->ctl[p->focus], (SInt16)((ev->message >> 8) & 0xFF), (SInt16)ch, ev->modifiers);
    return true;
}

static bool pw_edit(MacWin *mw, int item)
{
    PrefsWin *p = (PrefsWin *)mw;
    TEHandle te = p->focus != AU_PREF_NONE && pane_of(p->focus) == p->pane ? field_te(p, p->focus) : NULL;
    if (!te)
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
    PrefsWin *p = (PrefsWin *)mw;
    if (p->pane == AU_PREF_SECTION_BLOCKING && prefs_blocking_due()) {
        GrafPtr saved;
        GetPort(&saved);
        SetPort((GrafPtr)mw->win);
        blocking_changed(p);
        SetPort(saved);
    }
    if (mw->win == FrontWindow() && g_in_front && p->focus != AU_PREF_NONE) {
        SetPort((GrafPtr)mw->win);
        IdleControls(mw->win);
    }
}

/* What the session needs to know about this Mac. */
static AuPrefsEdit *new_session(void)
{
    AuPrefsHost h;
    memset(&h, 0, sizeof h);
    h.fonts = g_fonts;
    h.memory_free_kb = mac_memory_free_kb();
    h.blocking = mac_blocking_manager();
    h.default_browser = default_browser_state();
    h.machine = "this Mac";
    h.settings_name = "Internet Config";
    h.folder_example = "Macintosh HD:Downloads";
    h.no_folder = "the desktop";
    /* Empty means Internet Config's download folder, when set. */
    char *ic_folder = NULL;
    FSSpec dl;
    if (mac_ic_download_folder(&dl))
        ic_folder = mac_roman_to_utf8((const char *)dl.name + 1, dl.name[0]);
    h.system_downloads = ic_folder;
    AuPrefsEdit *e = au_prefs_edit_new(&g_browser->prefs, &h);
    au_free(ic_folder);
    return e;
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
    p->focus = AU_PREF_NONE;
    p->last_click = AU_PREF_NONE;
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
    p->edit = new_session();
    /* The tab titles are the 'tab#' resource's, in section order. */
    p->tab = make(p, -1, kAuControlTabSmallProc, 10, 10, kW - 20, kH - 60, NULL, 128, 0, 0);
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
    p->cancel = make(p, -1, kAuControlPushButtonProc, kW - 190, kH - 36, 80, 20, "Cancel", 0, 0, 0);
    p->ok = make(p, -1, kAuControlPushButtonProc, kW - 98, kH - 36, 80, 20, "OK", 0, 0, 0);
    Boolean yes = true;
    SetControlData(p->ok, 0, kAuControlPushButtonDefaultTag, sizeof yes, (Ptr)&yes);
    load(p);
    show_pane(p, 0);
    ShowWindow(win);
    SelectWindow(win);
    mac_menus_dirty();
}
