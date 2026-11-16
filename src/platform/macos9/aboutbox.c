/* The About box (Apple menu > About Aura): the kookaburra, the name and
 * version, what Aura is, a link to the home page, who made it, the
 * libraries it is built on, the Mac it runs on, and a quip. A click on
 * the kookaburra shows the next quip; a click on the link opens the home
 * page. Credits & Licences opens about:licences, which lists every
 * bundled font, library and data set (src/browser/credits.c). */
#include "mac.h"

#include "browser/credits.h"
#include "browser/sysinfo.h"

#include <stdio.h>
#include <string.h>

enum { A_OK = 1, A_LICENCES, A_ICON, A_HEAD, A_LINK, A_INFO };
enum { kAboutDialog = 202, kIconFamily = 128 };

static const char *const quips[] = {
    "The kookaburra laughs at 1 GHz.",
    "Bringing the modern web to Macs older than some of the people who write it.",
    "Kookaburras eat snakes. Aura eats JavaScript.",
    "Best viewed on a Mac with handles.",
    "Rendering the web one pixel at a time, which is how it was always done.",
    "No megabytes were wasted in the making of this browser. Well, a few.",
    "Platinum appearance, modern web, same old beige.",
    "Something broke? Develop > Report a Problem writes it up for you.",
};
enum { kQuips = (int)(sizeof quips / sizeof quips[0]) };

static int quip;
static char libs[200];
static char machine[200];

/* Draws UTF-8 text (converted to Mac Roman) wrapped in the box r, in the
 * port's current font. */
static void draw_wrapped(const char *utf8, const Rect *r)
{
    size_t n = 0;
    char *s = mac_utf8_to_roman(utf8, &n);
    if (!s)
        return;
    Rect box = *r;
    TETextBox(s, (long)n, &box, teFlushLeft);
    au_free(s);
}

static void draw_line(const char *utf8, short x, short y)
{
    Str255 p;
    mac_pstr(utf8, p);
    MoveTo(x, y);
    DrawString(p);
}

static void set_text(short font, short size, Style face, unsigned short grey)
{
    RGBColor c = {grey, grey, grey};
    TextFont(font);
    TextSize(size);
    TextFace(face);
    RGBForeColor(&c);
}

static void item_rect(DialogPtr d, short item, Rect *r)
{
    short type;
    Handle h;
    GetDialogItem(d, item, &type, &h, r);
}

/* The icon at twice its size: the 32 by 32 colour icon scaled by whole
 * pixels keeps its shapes crisp. */
static pascal void draw_icon(DialogPtr d, short item)
{
    Rect r;
    item_rect(d, item, &r);
    PlotIconID(&r, atAbsoluteCenter, ttNone, kIconFamily);
}

static pascal void draw_head(DialogPtr d, short item)
{
    Rect r;
    item_rect(d, item, &r);
    EraseRect(&r);
    set_text(systemFont, 24, bold, 0);
    draw_line("Aura", r.left, (short)(r.top + 24));
    char ver[80];
    snprintf(ver, sizeof ver, "Version %s", au_build_identity());
    set_text(applFont, 10, normal, 0x6000);
    draw_line(ver, r.left, (short)(r.top + 42));
    set_text(applFont, 12, normal, 0);
    Rect t = {(short)(r.top + 50), r.left, r.bottom, r.right};
    draw_wrapped(AU_TAGLINE, &t);
    set_text(systemFont, 12, normal, 0);
}

static pascal void draw_link(DialogPtr d, short item)
{
    Rect r;
    item_rect(d, item, &r);
    EraseRect(&r);
    RGBColor blue = {0x0000, 0x3333, 0xCCCC};
    TextFont(applFont);
    TextSize(12);
    TextFace(underline);
    RGBForeColor(&blue);
    draw_line("github.com/matjam/aura", r.left, (short)(r.top + 12));
    set_text(systemFont, 12, normal, 0);
}

static pascal void draw_info(DialogPtr d, short item)
{
    Rect r;
    item_rect(d, item, &r);
    EraseRect(&r);
    /* A rule above the facts. */
    RGBColor rule = {0xBBBB, 0xBBBB, 0xBBBB};
    RGBForeColor(&rule);
    MoveTo(r.left, r.top);
    LineTo(r.right, r.top);

    set_text(applFont, 12, normal, 0);
    draw_line("Written by " AU_AUTHOR ", with help from Claude.", r.left, (short)(r.top + 20));
    set_text(applFont, 10, normal, 0x3000);
    Rect t = {(short)(r.top + 28), r.left, (short)(r.top + 56), r.right};
    draw_wrapped(libs, &t);
    t.top = (short)(r.top + 58);
    t.bottom = (short)(r.top + 86);
    draw_wrapped(machine, &t);
    set_text(applFont, 12, italic, 0x4000);
    t.top = (short)(r.top + 90);
    t.bottom = r.bottom;
    draw_wrapped(quips[quip], &t);
    set_text(systemFont, 12, normal, 0);
}

/* "QuickJS-ng 0.18.0 and Mbed TLS 4.1.1 with TF-PSA-Crypto 1.1.1": the
 * names of the libraries in the credits table. */
static void library_names(char *out, size_t cap)
{
    size_t n = 0, count = 0, total = 0;
    out[0] = 0;
    for (size_t i = 0; i < au_ncredits; i++)
        total += au_credits[i].kind == AU_CREDIT_LIBRARY;
    for (size_t i = 0; i < au_ncredits && n + 1 < cap; i++) {
        const AuCredit *c = &au_credits[i];
        if (c->kind != AU_CREDIT_LIBRARY)
            continue;
        const char *sep = count == 0 ? "" : count + 1 == total ? " and " : ", ";
        int k = snprintf(out + n, cap - n, "%s%s", sep, c->name);
        if (k < 0 || (size_t)k >= cap - n)
            break;
        n += (size_t)k;
        count++;
    }
}

static void describe_machine(char *out, size_t cap)
{
    AuSysInfo s;
    mac_sysinfo(&s);
    const char *what = s.model[0] ? s.model : s.cpu[0] ? s.cpu : "a PowerPC Mac";
    char speed[24] = "", ram[24] = "";
    if (s.cpu_mhz)
        snprintf(speed, sizeof speed, " at %lu MHz", (unsigned long)s.cpu_mhz);
    if (s.ram_mb)
        snprintf(ram, sizeof ram, " with %lu MB", (unsigned long)s.ram_mb);
    snprintf(out, cap, "Running on %s%s%s, %s.", what, speed, ram, s.os[0] ? s.os : "Mac OS");
}

static void redraw(DialogPtr d, short item)
{
    Rect r;
    item_rect(d, item, &r);
    SetPort((GrafPtr)d);
    InvalRect(&r);
}

void mac_about_box(void)
{
    char names[160];
    library_names(names, sizeof names);
    if (names[0])
        snprintf(libs, sizeof libs, "Built with %s. Fonts and data are listed under Credits & Licences.", names);
    else
        snprintf(libs, sizeof libs, "%s", "Fonts, libraries and data are listed under Credits & Licences.");
    describe_machine(machine, sizeof machine);
    quip = (int)((unsigned long)TickCount() % (unsigned long)kQuips);

    DialogPtr d = GetNewDialog(kAboutDialog, NULL, (WindowPtr)-1);
    if (!d)
        return;
    static UserItemUPP icon_upp, head_upp, link_upp, info_upp;
    if (!icon_upp) {
        icon_upp = NewUserItemUPP(draw_icon);
        head_upp = NewUserItemUPP(draw_head);
        link_upp = NewUserItemUPP(draw_link);
        info_upp = NewUserItemUPP(draw_info);
    }
    const struct {
        short item;
        UserItemUPP upp;
    } users[] = {{A_ICON, icon_upp}, {A_HEAD, head_upp}, {A_LINK, link_upp}, {A_INFO, info_upp}};
    for (size_t i = 0; i < sizeof users / sizeof users[0]; i++) {
        short type;
        Handle h;
        Rect r;
        GetDialogItem(d, users[i].item, &type, &h, &r);
        SetDialogItem(d, users[i].item, type, (Handle)users[i].upp, &r);
    }
    SetDialogDefaultItem(d, A_OK);
    SetDialogCancelItem(d, A_OK);
    ShowWindow((WindowPtr)d);
    short item = 0;
    for (;;) {
        ModalDialog(NULL, &item);
        if (item == A_ICON) {
            quip = (quip + 1) % kQuips;
            redraw(d, A_INFO);
            continue;
        }
        if (item == A_OK || item == A_LICENCES || item == A_LINK)
            break;
    }
    DisposeDialog(d);
    if (item == A_LICENCES)
        mac_open_url("about:licences", true);
    else if (item == A_LINK)
        mac_open_url(AU_HOMEPAGE, true);
}
