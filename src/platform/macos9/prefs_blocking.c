/* The Blocking pane's list rows (see blocking_mac.h): one check box per
 * catalogue list with a status line beside it, built from the rows the
 * editing session gives (au_prefs_edit_block_list, prefs_ui.h). The
 * rest of the pane is built by prefswin.c from the descriptions.
 *
 * The check boxes act at once (prefswin.c applies them as live items);
 * Cancel puts the lists back. The status lines are redrawn from
 * prefswin.c's idle when the manager's generation changes, and once a
 * second while it downloads. */
#include "blocking_mac.h"

#include "browser/prefs_ui.h"
#include "clock_mac.h"

#include <stdio.h>
#include <string.h>

enum { kMaxRows = 8 };

static struct {
    WindowPtr win;
    AuPrefsEdit *edit;
    int nrows;
    ControlHandle box[kMaxRows], status[kMaxRows];
    char shown[kMaxRows][256]; /* the status text on screen */
    uint32_t generation, last_ms;
} P;

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

short prefs_blocking_build(WindowPtr win, ControlHandle pane, AuPrefsEdit *edit, short top)
{
    memset(&P, 0, sizeof P);
    P.win = win;
    P.edit = edit;
    size_t n = au_prefs_edit_block_list_count(edit);
    short y = top;
    AuPrefBlockRow row;
    for (size_t i = 0; i < n && P.nrows < kMaxRows && au_prefs_edit_block_list(edit, i, &row); i++) {
        int r = P.nrows++;
        P.box[r] = make(pane, kAuControlCheckBoxAutoToggleProc, 30, y, 296, 18, row.title, 0, 0, 1);
        P.status[r] = small_text(pane, 330, (short)(y + 3), 204);
        y += 18;
    }
    return (short)(y + 8);
}

void prefs_blocking_load(void)
{
    AuPrefBlockRow row;
    for (int r = 0; r < P.nrows; r++)
        if (au_prefs_edit_block_list(P.edit, (size_t)r, &row))
            SetControlValue(P.box[r], row.on);
    prefs_blocking_refresh(false);
}

void prefs_blocking_refresh(bool draw)
{
    if (!P.win || !mac_blocking_manager())
        return;
    AuPrefBlockRow row;
    for (int r = 0; r < P.nrows; r++) {
        if (!au_prefs_edit_block_list(P.edit, (size_t)r, &row) || !strcmp(row.status, P.shown[r]))
            continue;
        snprintf(P.shown[r], sizeof P.shown[r], "%s", row.status);
        set_static(P.status[r], row.status, draw);
    }
    P.generation = au_prefs_edit_block_generation(P.edit);
}

bool prefs_blocking_click(ControlHandle c)
{
    if (!P.win)
        return false;
    for (int r = 0; r < P.nrows; r++)
        if (c == P.box[r])
            return au_prefs_edit_set_block_list(P.edit, (size_t)r, GetControlValue(c) != 0);
    return false;
}

bool prefs_blocking_due(void)
{
    AuBlockManager *m = mac_blocking_manager();
    if (!m || !P.win)
        return false;
    uint32_t now = au_mac_clock_ms();
    if (au_prefs_edit_block_generation(P.edit) == P.generation && !(mac_blocking_busy() && now - P.last_ms >= 1000))
        return false;
    P.last_ms = now;
    return true;
}

void prefs_blocking_close(void)
{
    memset(&P, 0, sizeof P);
}
