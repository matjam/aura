/* The Blocking pane of the Preferences window (see blocking_mac.h).
 *
 * One checkbox per catalogue list with a status line beside it (its size
 * and date, or what the update manager is doing with it), the update
 * interval, Update Now, and a status line for the lists added by address.
 * prefswin.c makes the switch, the three edit fields and their labels.
 *
 * The list checkboxes and Update Now act at once, so the status lines
 * show the download; Cancel puts the lists back as they were. The status
 * lines are redrawn from prefs_blocking_idle when the manager's
 * generation changes (and once a second while it downloads). */
#include "blocking_mac.h"

#include "browser/history.h" /* dates */
#include "clock_mac.h"
#include "net/blocklist.h"

#include <stdio.h>
#include <string.h>

enum { kMaxRows = 8, M_BLOCK_UPDATE = 155 };

static struct {
    WindowPtr win;
    int nrows;
    char id[kMaxRows][32];
    ControlHandle box[kMaxRows], status[kMaxRows];
    ControlHandle interval, update_now, custom_status;
    char shown[kMaxRows + 1][256]; /* the status text on screen */
    uint32_t generation, last_ms;
    char *orig_lists;
    bool orig_on;
} P;

static const char *const month_abbr[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static ControlHandle make(ControlHandle pane, short proc, short x, short y, short w, short h, const char *title,
                          short value, short min, short max)
{
    Rect r;
    SetRect(&r, x, y, (short)(x + w), (short)(y + h));
    Str255 t;
    mac_pstr(title ? title : "", t);
    ControlHandle c = NewControl(P.win, &r, t, true, value, min, max, proc, 0);
    if (!c)
        mac_out_of_memory(512);
    EmbedControl(c, pane);
    return c;
}

static ControlHandle small_text(ControlHandle pane, short x, short y, short w)
{
    ControlHandle c = make(pane, kAuControlStaticTextProc, x, y, w, 14, NULL, 0, 0, 0);
    AuControlFontStyleRec st;
    memset(&st, 0, sizeof st);
    st.flags = kAuControlUseFontMask;
    st.font = kAuControlFontSmallSystemFont;
    SetControlFontStyle(c, &st);
    return c;
}

static void set_static(ControlHandle c, const char *utf8, bool draw)
{
    size_t n;
    char *r = mac_utf8_to_roman(utf8, &n);
    SetControlData(c, 0, kAuControlStaticTextTextTag, (Size)n, r);
    au_free(r);
    if (draw)
        Draw1Control(c);
}

/* "91,414". */
static void group_digits(char *out, size_t cap, unsigned long v)
{
    char raw[24];
    int n = snprintf(raw, sizeof raw, "%lu", v);
    size_t k = 0;
    for (int i = 0; i < n && k + 2 < cap; i++) {
        if (i && (n - i) % 3 == 0)
            out[k++] = ',';
        out[k++] = raw[i];
    }
    out[k] = 0;
}

static void describe(const AuBlockListInfo *li, char *out, size_t cap)
{
    char count[24];
    group_digits(count, sizeof count, li->entries);
    if (li->state == AU_BLOCK_DOWNLOADING) {
        snprintf(out, cap, "downloading, %ld KB so far", (long)(li->received / 1024));
    } else if (li->state == AU_BLOCK_COMPILING) {
        snprintf(out, cap, "compiling");
    } else if (li->error[0] && !li->loaded) {
        snprintf(out, cap, "not loaded: %s", li->error);
    } else if (li->state == AU_BLOCK_WAITING && !li->loaded) {
        snprintf(out, cap, "waiting to download");
    } else if (li->loaded) {
        int y, m, d, wd;
        au_date_of_day(au_day_of(li->updated_at), &y, &m, &d, &wd);
        if (li->updated_at)
            snprintf(out, cap, "%s domains, %lu KB, %d %s%s%s", count, (unsigned long)((li->bytes + 512) / 1024), d,
                     month_abbr[(m + 11) % 12], li->error[0] ? "; " : "", li->error);
        else
            snprintf(out, cap, "%s domains, %lu KB", count, (unsigned long)((li->bytes + 512) / 1024));
    } else {
        snprintf(out, cap, "about %s domains, %lu KB", count,
                 (unsigned long)(((unsigned long)li->entries * AU_BLOCK_HASH_BYTES + 512) / 1024));
    }
}

static void refresh(bool draw)
{
    AuBlockManager *m = mac_blocking_manager();
    if (!m || !P.win)
        return;
    AuBlockListInfo li;
    unsigned long custom = 0, custom_loaded = 0, custom_domains = 0;
    char custom_error[96] = "", custom_doing[48] = "";
    for (size_t i = 0; au_block_manager_info(m, i, &li); i++) {
        if (!li.catalog) {
            custom++;
            if (li.loaded) {
                custom_loaded++;
                custom_domains += li.entries;
            }
            if (li.error[0] && !custom_error[0])
                snprintf(custom_error, sizeof custom_error, "%s", li.error);
            if (li.state == AU_BLOCK_DOWNLOADING || li.state == AU_BLOCK_COMPILING)
                snprintf(custom_doing, sizeof custom_doing, "; %s",
                         li.state == AU_BLOCK_COMPILING ? "compiling" : "downloading");
            continue;
        }
        for (int r = 0; r < P.nrows; r++) {
            if (strcmp(P.id[r], li.id))
                continue;
            char text[256];
            describe(&li, text, sizeof text);
            if (strcmp(text, P.shown[r])) {
                snprintf(P.shown[r], sizeof P.shown[r], "%s", text);
                set_static(P.status[r], text, draw);
            }
        }
    }
    char text[256];
    if (!custom) {
        snprintf(text, sizeof text, "Addresses of other lists (https), separated by spaces.");
    } else {
        char count[24];
        group_digits(count, sizeof count, custom_domains);
        snprintf(text, sizeof text, "%lu of %lu loaded, %s domains%s%s%s", custom_loaded, custom, count, custom_doing,
                 custom_error[0] ? "; " : "", custom_error);
    }
    if (strcmp(text, P.shown[kMaxRows])) {
        snprintf(P.shown[kMaxRows], sizeof P.shown[kMaxRows], "%s", text);
        set_static(P.custom_status, text, draw);
    }
    P.generation = au_block_manager_generation(m);
}

void prefs_blocking_build(WindowPtr win, ControlHandle pane, short top)
{
    memset(&P, 0, sizeof P);
    P.win = win;
    size_t n;
    const AuBlockCatalogEntry *cat = au_block_catalog(&n);
    short y = top;
    for (size_t i = 0; i < n && P.nrows < kMaxRows; i++) {
        int r = P.nrows++;
        snprintf(P.id[r], sizeof P.id[r], "%s", cat[i].id);
        P.box[r] = make(pane, kAuControlCheckBoxAutoToggleProc, 30, y, 296, 18, cat[i].title, 0, 0, 1);
        P.status[r] = small_text(pane, 330, (short)(y + 3), 204);
        y += 18;
    }
    y += 8;
    ControlHandle lab = make(pane, kAuControlStaticTextProc, 30, (short)(y + 2), 110, 16, NULL, 0, 0, 0);
    set_static(lab, "Look for updates:", false);
    MenuHandle m = GetMenuHandle(M_BLOCK_UPDATE);
    if (m) {
        DeleteMenu(M_BLOCK_UPDATE);
        DisposeMenu(m);
    }
    m = NewMenu(M_BLOCK_UPDATE, (ConstStr255Param) "\p");
    if (!m)
        mac_out_of_memory(256);
    mac_append_item(m, "Every day");
    mac_append_item(m, "Every week");
    mac_append_item(m, "Never");
    InsertMenu(m, -1);
    P.interval = make(pane, kAuControlPopupButtonProc, 144, y, 130, 20, NULL, 0, M_BLOCK_UPDATE, 0);
    SetControlMaximum(P.interval, 3);
    P.update_now = make(pane, kAuControlPushButtonProc, 290, y, 110, 20, "Update Now", 0, 0, 0);
    /* Under prefswin.c's "More lists" field, which is the next row. */
    P.custom_status = small_text(pane, 144, (short)(y + 28 + 22), 390);
}

void prefs_blocking_load(const AuPrefs *pr)
{
    for (int r = 0; r < P.nrows; r++)
        SetControlValue(P.box[r], au_block_words_has(pr->block_lists, P.id[r]));
    SetControlValue(P.interval, (short)(pr->block_update + 1));
    au_free(P.orig_lists);
    P.orig_lists = au_strndup(pr->block_lists, strlen(pr->block_lists));
    P.orig_on = pr->block_trackers;
    refresh(false);
}

bool prefs_blocking_click(ControlHandle c)
{
    if (!P.win)
        return false;
    AuPrefs *pr = &g_browser->prefs;
    if (c == P.update_now) {
        AuBlockManager *m = mac_blocking_manager();
        if (m)
            au_block_manager_update_now(m);
        refresh(true);
        return true;
    }
    if (c == P.interval)
        return true; /* stored on OK */
    for (int r = 0; r < P.nrows; r++) {
        if (c != P.box[r])
            continue;
        AuBuf b;
        au_buf_init(&b);
        for (int k = 0; k < P.nrows; k++)
            if (GetControlValue(P.box[k])) {
                if (b.len)
                    au_buf_putc(&b, ' ');
                au_buf_puts(&b, P.id[k]);
            }
        au_prefs_set_str(&pr->block_lists, au_buf_cstr(&b));
        au_buf_free(&b);
        g_browser->prefs_gen++;
        mac_blocking_prefs_changed();
        refresh(true);
        return true;
    }
    return false;
}

void prefs_blocking_idle(void)
{
    AuBlockManager *m = mac_blocking_manager();
    if (!m || !P.win)
        return;
    uint32_t now = au_mac_clock_ms();
    if (au_block_manager_generation(m) == P.generation && !(mac_blocking_busy() && now - P.last_ms >= 1000))
        return;
    P.last_ms = now;
    GrafPtr saved;
    GetPort(&saved);
    SetPort((GrafPtr)P.win);
    refresh(true);
    SetPort(saved);
}

/* Appends the https addresses in text to out, separated by spaces. */
static void clean_urls(const char *text, AuBuf *out)
{
    for (const char *s = text; *s;) {
        while (*s && (unsigned char)*s <= ' ')
            s++;
        size_t n = 0;
        while (s[n] && (unsigned char)s[n] > ' ')
            n++;
        if (n > 11 && n < 1024 && au_ieq(s, 8, "https://", 8)) {
            if (out->len)
                au_buf_putc(out, ' ');
            au_buf_append(out, s, n);
        }
        s += n;
    }
}

/* The same for domains (commas also separate). */
static void clean_domains(const char *text, AuBuf *out)
{
    for (const char *s = text; *s;) {
        while (*s && ((unsigned char)*s <= ' ' || *s == ','))
            s++;
        size_t n = 0;
        while (s[n] && (unsigned char)s[n] > ' ' && s[n] != ',')
            n++;
        AuBuf d;
        au_buf_init(&d);
        if (n && au_block_clean_domain(s, n, &d) && !au_block_words_has(au_buf_cstr(out), au_buf_cstr(&d))) {
            if (out->len)
                au_buf_putc(out, ' ');
            au_buf_append(out, d.data, d.len);
        }
        au_buf_free(&d);
        s += n;
    }
}

void prefs_blocking_store(AuPrefs *pr, const char *urls, const char *allow, const char *block)
{
    int v = GetControlValue(P.interval) - 1;
    pr->block_update = v == 1 ? AU_BLOCK_UPDATE_WEEKLY : v == 2 ? AU_BLOCK_UPDATE_NEVER : AU_BLOCK_UPDATE_DAILY;
    AuBuf b;
    au_buf_init(&b);
    clean_urls(urls, &b);
    au_prefs_set_str(&pr->block_custom, au_buf_cstr(&b));
    au_buf_clear(&b);
    clean_domains(allow, &b);
    au_prefs_set_str(&pr->block_allow, au_buf_cstr(&b));
    au_buf_clear(&b);
    clean_domains(block, &b);
    au_prefs_set_str(&pr->block_deny, au_buf_cstr(&b));
    au_buf_free(&b);
}

void prefs_blocking_cancel(AuPrefs *pr)
{
    if (!P.orig_lists)
        return;
    if (strcmp(pr->block_lists, P.orig_lists) || pr->block_trackers != P.orig_on) {
        au_prefs_set_str(&pr->block_lists, P.orig_lists);
        pr->block_trackers = P.orig_on;
        g_browser->prefs_gen++;
        mac_blocking_prefs_changed();
    }
}

void prefs_blocking_close(void)
{
    MenuHandle m = GetMenuHandle(M_BLOCK_UPDATE);
    if (m) {
        DeleteMenu(M_BLOCK_UPDATE);
        DisposeMenu(m);
    }
    au_free(P.orig_lists);
    memset(&P, 0, sizeof P);
}
