/* The About box (Apple menu > About Aura): the kookaburra, the version,
 * what Aura is, and the fonts and libraries it uses, from the credits
 * table (src/browser/credits.c). Licences opens about:licences. */
#include "mac.h"

#include "browser/credits.h"

#include <stdio.h>
#include <string.h>

enum { A_OK = 1, A_LICENCES, A_ICON, A_TITLE, A_TAGLINE, A_FONTS, A_LIBS };
enum { kAboutDialog = 202, kIconFamily = 128 };

/* The icon at twice its size: the 32 by 32 colour icon scaled by whole
 * pixels keeps its shapes crisp. */
static pascal void draw_icon(DialogPtr d, short item)
{
    short type;
    Handle h;
    Rect r;
    GetDialogItem(d, item, &type, &h, &r);
    PlotIconID(&r, atAbsoluteCenter, ttNone, kIconFamily);
}

/* "A, B, C and D" from the names of one kind of credit, each name cut
 * before " (" so the line stays short. */
static void list_names(AuCreditKind kind, char *out, size_t cap)
{
    size_t n = 0, count = 0, total = 0;
    out[0] = 0;
    for (size_t i = 0; i < au_ncredits; i++)
        total += au_credits[i].kind == kind;
    for (size_t i = 0; i < au_ncredits && n + 1 < cap; i++) {
        const AuCredit *c = &au_credits[i];
        if (c->kind != kind)
            continue;
        const char *sep = count == 0 ? "" : count + 1 == total ? " and " : ", ";
        size_t len = strcspn(c->name, "(");
        while (len && c->name[len - 1] == ' ')
            len--;
        int k = snprintf(out + n, cap - n, "%s%.*s", sep, (int)len, c->name);
        if (k < 0)
            break;
        n += (size_t)k;
        count++;
    }
}

void mac_about_box(void)
{
    /* Each line becomes a Pascal string (255 bytes at most, cut there). */
    char fonts[300], libs[300], names[200];
    list_names(AU_CREDIT_FONT, names, sizeof names);
    snprintf(fonts, sizeof fonts, "Fonts: %s. Data: Mozilla's root certificates and the Public Suffix List.",
             names);
    list_names(AU_CREDIT_LIBRARY, names, sizeof names);
    if (names[0])
        snprintf(libs, sizeof libs, "Libraries: %s.", names);
    else
        snprintf(libs, sizeof libs, "%s", "Libraries: none in this build.");
    Str255 p0, p1, p2, p3;
    mac_pstr("Aura " AU_VERSION, p0);
    mac_pstr(AU_TAGLINE, p1);
    mac_pstr(fonts, p2);
    mac_pstr(libs, p3);
    ParamText(p0, p1, p2, p3);
    DialogPtr d = GetNewDialog(kAboutDialog, NULL, (WindowPtr)-1);
    if (!d)
        return;
    static UserItemUPP icon_upp;
    if (!icon_upp)
        icon_upp = NewUserItemUPP(draw_icon);
    short type;
    Handle h;
    Rect r;
    GetDialogItem(d, A_ICON, &type, &h, &r);
    SetDialogItem(d, A_ICON, userItem, (Handle)icon_upp, &r);
    SetDialogDefaultItem(d, A_OK);
    SetDialogCancelItem(d, A_OK);
    ShowWindow((WindowPtr)d);
    short item = 0;
    do {
        ModalDialog(NULL, &item);
    } while (item != A_OK && item != A_LICENCES);
    DisposeDialog(d);
    if (item == A_LICENCES)
        mac_open_url("about:licences", true);
}
