/* Input while listeners destroy what the input is for.
 *
 * Each case loads a page of form controls with script on, installs one
 * listener (capturing, on the document, once) for one event type that
 * destroys something (the event's target, its form, an ancestor, the
 * whole body, the document's content, the document element; or moves
 * the target to another document, navigates, or removes the frame the
 * event is in) and then drives one control with the mouse, the keyboard
 * and the Edit commands. The document view fires the event from C and
 * goes on working afterwards: it must hold what it still uses and ask
 * whether it is still in the document (docs/conventions.md, "Pointers
 * across script").
 *
 * The test asserts little: it is run under AddressSanitizer by ctest in
 * the sanitizer tree, where a use of a freed node aborts it. Each
 * (event, destroyer, control) combination runs in a fresh view, at the
 * top and inside an iframe.
 *
 * usage: test_input_lifetime                    a part of the matrix (ctest)
 *        test_input_lifetime all                all of it: 10,710 cases
 *        test_input_lifetime [all] EVENT [DESTROYER [CONTROL]]
 *                                               indices, to run cases again
 * AURA_TEST_VERBOSE=1 prints each case before it runs (the last one
 * printed is the one that failed); AURA_TEST_GC_STRESS=1 collects script
 * garbage whenever script is left. After a change to input handling, run
 * `all` with AURA_TEST_GC_STRESS=1 in the sanitizer tree, one process
 * per event (docs/fuzzing.md gives the command). */
#include "../layout/ahem.h"
#include "js/js.h"
#include "page/page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static AuFontSet *fonts;
static int cases, submits, opens;

static const char controls_html[] =
    "<div id=wrap><form id=f action=/s>"
    "<input id=t name=t value=abc><input id=t2 name=t2 value=q>"
    "<input type=checkbox id=cb name=cb><input type=radio name=r id=r1><input type=radio name=r id=r2>"
    "<select id=se name=se><option>x</option><option>y</option></select>"
    "<select id=lb size=3 multiple><option>a</option><option>b</option><option>c</option></select>"
    "<input type=range id=rg><input type=file id=fi>"
    "<textarea id=ta>text</textarea>"
    "<details id=dt><summary id=sm><b id=smb>s</b></summary>d</details>"
    "<label id=la for=t2><span id=las>label</span></label><label id=lc for=cb><i id=lci>check</i></label>"
    "<a id=ln href='#x'><i id=lni>link</i></a><span id=sp tabindex=0><i id=spi>span</i></span>"
    "<button id=bu><b id=bub>go</b></button><input type=reset id=rs></form>"
    "<form id=g action=/g><input id=rq name=rq required><input id=rq2 name=rq2 required>"
    "<button id=bg><b id=bgb>go</b></button></form>"
    "<form id=h action=/h><input id=only name=only value=v></form>"
    "</div><p id=tail>tail</p>";

/* Where the pointer goes. Several are elements inside a control (the
 * text of a button, a link, a label, a summary): the hit test names the
 * inner element, the control above it gets the focus, and a listener
 * that empties the control frees the element the press was on. */
static const char *const control_ids[] = {"t",   "cb",  "r1",  "se",  "lb",  "rg",  "fi", "ta",  "smb",
                                          "las", "lci", "lni", "spi", "bub", "rs", "bgb", "only"};
#define NCONTROLS ((int)(sizeof control_ids / sizeof control_ids[0]))

static const char *const event_types[] = {"focus",     "blur",    "focusin", "focusout", "mousedown", "mouseup", "click",
                                          "mousemove", "mouseover", "mouseout", "keydown", "keypress", "keyup",
                                          "beforeinput", "input",  "change",  "submit",   "invalid",   "reset",
                                          "select",    "toggle"};
#define NEVENTS ((int)(sizeof event_types / sizeof event_types[0]))

/* What the listener does; D is the document the controls are in. */
static const char *const destroyers[] = {
    "ev.target.remove()",
    "var fo=ev.target.form||ev.target.closest('form');if(fo)fo.remove();else ev.target.remove()",
    "D.getElementById('wrap').remove()",
    "D.body.innerHTML='<p>gone</p><input id=t>'",
    "D.open();D.write('<p>new</p><input id=t autofocus>');D.close()",
    "D.documentElement.remove()",
    "D.implementation.createHTMLDocument('').body.appendChild(ev.target)",
    "location.href='http://example.com/next'",
    "var fe=window.frameElement;if(fe)fe.remove();else D.body.remove()",
    "ev.target.parentNode.replaceChildren()",
    "var a=D.activeElement;if(a&&a!==D.body)a.remove();D.getElementById('t2').focus();ev.target.remove()",
    /* Emptying an element frees its children at once when nothing else
     * holds them, the element under the pointer among them. */
    "ev.target.textContent=''",
    "var a=D.activeElement;if(a)a.innerHTML='';ev.target.innerHTML=''",
    "D.querySelectorAll('b,i,span,option,summary,label').forEach(function(x){x.remove()})",
    "D.querySelectorAll('input,select,textarea,button,a,form').forEach(function(x){x.remove()})",
};
#define NDESTROYERS ((int)(sizeof destroyers / sizeof destroyers[0]))
/* The destroyers the short run gives every event and control: an
 * ancestor removed, the document written again, the controls emptied. */
enum { QUICK_DS_A = 2, QUICK_DS_B = 4, QUICK_DS_C = 12 };

static void cb_submit(void *ctx, AuDocView *dv, const AuFormSubmission *s)
{
    (void)ctx, (void)dv, (void)s;
    submits++;
}

static void cb_open(void *ctx, AuDocView *dv, const char *url, bool new_tab)
{
    (void)ctx, (void)dv, (void)url, (void)new_tab;
    opens++;
}

static int cb_popup(void *ctx, AuDocView *dv, const AuPopupItem *items, int n, int selected, AuIRect at)
{
    (void)ctx, (void)dv, (void)items, (void)at;
    return n > 1 ? (selected + 1) % n : -1;
}

static bool cb_choose_file(void *ctx, AuDocView *dv, bool multiple, AuControlFile **files, uint32_t *n)
{
    (void)ctx, (void)dv, (void)multiple;
    AuControlFile *f = au_calloc(sizeof *f);
    f->name = au_strndup("a.txt", 5);
    f->type = au_strndup("text/plain", 10);
    f->data = au_strndup("data", 4);
    f->len = 4;
    *files = f;
    *n = 1;
    return true;
}

static bool cb_confirm(void *ctx, AuDocView *dv, const char *message)
{
    (void)ctx, (void)dv, (void)message;
    return true;
}

static void settle(AuDocView *dv)
{
    for (int i = 0; i < 3; i++)
        au_docview_run_scripts(dv, 10);
    au_docview_update(dv);
}

/* The element with that id in the document the controls are in: the
 * view's own, or the one nested in its iframe. *view gets its view and
 * (*ox, *oy) the offset of that view in the top one. */
static AuElement *find(AuDocView *top, bool framed, const char *id, AuDocView **view, int *ox, int *oy)
{
    *ox = *oy = 0;
    *view = top;
    au_docview_update(top);
    if (!framed)
        return au_docview_document(top) ? au_docview_find_fragment(top, id) : NULL;
    AuElement *fe = au_docview_document(top) ? au_docview_find_fragment(top, "fr") : NULL;
    AuRect r;
    if (!fe || !au_layout_element_rect(au_docview_layout(top), fe, &r))
        return NULL;
    /* a point inside the frame names its view */
    int x = (int)r.x + 4, y = (int)r.y + 4;
    AuDocView *inner = au_docview_view_at(top, &x, &y);
    if (!inner || inner == top || !au_docview_document(inner))
        return NULL;
    au_docview_update(inner);
    *view = inner;
    *ox = (int)r.x + 4 - x;
    *oy = (int)r.y + 4 - y;
    return au_docview_find_fragment(inner, id);
}

/* The centre of the element in the top view's coordinates. */
static bool centre(AuDocView *top, bool framed, const char *id, int *x, int *y)
{
    int ox, oy;
    AuDocView *v;
    AuElement *el = find(top, framed, id, &v, &ox, &oy);
    if (!el)
        return false;
    AuRect r;
    if (!au_layout_element_rect(au_docview_layout(v), el, &r) || r.w <= 0 || r.h <= 0)
        return false;
    *x = ox + (int)(r.x + r.w / 2);
    *y = oy + (int)(r.y + r.h / 2);
    return true;
}

static void mouse(AuDocView *dv, int type, int x, int y, int clicks, uint32_t mods)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.x = x;
    e.y = y;
    e.clicks = clicks;
    e.mods = mods;
    au_docview_mouse(dv, &e);
}

static void key(AuDocView *dv, int k, uint32_t ch, uint32_t mods)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.type = AU_EVENT_KEY_DOWN;
    e.key = k;
    e.ch = ch;
    e.mods = mods;
    au_docview_key(dv, &e);
}

/* hover: the pointer moves onto the element first, as a user's does. The
 * view then holds the element as its hover target, which hides a missing
 * reference elsewhere: the first press of a case comes without it (the
 * pointer jumps there, as a fuzzer's does, or the page moved under it). */
static void press(AuDocView *dv, bool framed, const char *id, int clicks, uint32_t mods, bool hover)
{
    int x, y;
    if (!centre(dv, framed, id, &x, &y))
        return;
    if (hover)
        mouse(dv, AU_EVENT_MOUSE_MOVE, x, y, 0, 0);
    mouse(dv, AU_EVENT_MOUSE_DOWN, x, y, clicks, mods);
    mouse(dv, AU_EVENT_MOUSE_MOVE, x + 3, y + 2, 0, 0); /* a drag in a field, list box or slider */
    mouse(dv, AU_EVENT_MOUSE_UP, x + 3, y + 2, clicks, mods);
}

static void edit(AuDocView *top, int cmd, const char *text)
{
    AuDocView *v = au_docview_focus_view(top);
    AuBuf out;
    au_buf_init(&out);
    if (v && au_docview_editing(v))
        au_docview_edit(v, cmd, text, text ? strlen(text) : 0, &out);
    au_buf_free(&out);
}

static void drive(AuDocView *dv, bool framed, const char *id)
{
    static const int keys[] = {AU_KEY_BACKSPACE, AU_KEY_DOWN, AU_KEY_UP, AU_KEY_RIGHT, AU_KEY_LEFT,
                               AU_KEY_HOME,      AU_KEY_END,  AU_KEY_DELETE};
    press(dv, framed, id, 1, 0, false);
    key(dv, 0, 'a', 0);
    key(dv, 0, ' ', 0);
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++)
        key(dv, keys[i], 0, i == 1 ? AU_MOD_SHIFT : 0);
    edit(dv, AU_EDITCMD_PASTE, "zz");
    edit(dv, AU_EDITCMD_SELECT_ALL, NULL);
    edit(dv, AU_EDITCMD_CUT, NULL);
    edit(dv, AU_EDITCMD_UNDO, NULL);
    settle(dv);
    press(dv, framed, id, 2, 0, false);           /* a double click: a word in a field */
    press(dv, framed, id, 1, AU_MOD_SHIFT, true); /* extends a list box's selection */
    key(dv, AU_KEY_RETURN, 0, 0);           /* change, implicit submission, a button's click */
    key(dv, AU_KEY_TAB, 0, 0);
    key(dv, AU_KEY_TAB, 0, AU_MOD_SHIFT);
    key(dv, AU_KEY_ESCAPE, 0, 0);
    press(dv, framed, "t2", 1, 0, false); /* the focus leaves: change and blur */
    press(dv, framed, "tail", 1, 0, true);
    /* The pointer leaves, then one more round on whatever is there. */
    mouse(dv, AU_EVENT_MOUSE_MOVE, 790, 590, 0, 0);
    settle(dv);
    press(dv, framed, "t", 1, 0, false);
    key(dv, 0, 'b', 0);
    au_docview_set_focus(dv, NULL);
    settle(dv);
}

static void run_case(int ev, int ds, int ct, bool framed)
{
    AuBuf html;
    au_buf_init(&html);
    char script[1200];
    snprintf(script, sizeof script,
             /* The listener ends by asking for a layout: the box tree holds
              * a reference to each node it shows, so a removed node is
              * freed only once the tree has been built again. */
             "<script>var D=document;D.addEventListener('%s',function(ev){try{%s}catch(_){}"
             "try{(D.body||D.documentElement).offsetWidth}catch(_){}},{capture:true,once:true});"
             "</script>",
             event_types[ev], destroyers[ds]);
    if (framed) {
        /* The controls and the listener are in a nested document. */
        au_buf_puts(&html, "<!DOCTYPE html><p id=top>top</p><input id=outer><iframe id=fr width=700 height=420 srcdoc=\"");
        AuBuf inner;
        au_buf_init(&inner);
        au_buf_puts(&inner, "<!DOCTYPE html>");
        au_buf_puts(&inner, controls_html);
        au_buf_puts(&inner, script);
        for (size_t i = 0; i < inner.len; i++) {
            if (inner.data[i] == '"')
                au_buf_puts(&html, "&quot;");
            else if (inner.data[i] == '&')
                au_buf_puts(&html, "&amp;");
            else
                au_buf_putc(&html, inner.data[i]);
        }
        au_buf_free(&inner);
        au_buf_puts(&html, "\"></iframe><p id=after>after</p>");
    } else {
        au_buf_puts(&html, "<!DOCTYPE html>");
        au_buf_puts(&html, controls_html);
        au_buf_puts(&html, script);
    }
    AuDocViewCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.submit = cb_submit;
    cb.open_url = cb_open;
    cb.popup = cb_popup;
    cb.choose_file = cb_choose_file;
    cb.confirm = cb_confirm;
    AuDocView *dv = au_docview_new(fonts, NULL, &cb);
    au_docview_set_scripting(dv, true);
    au_docview_set_viewport(dv, 800, 600);
    au_docview_load(dv, html.data, html.len, "http://example.com/dir/page.html");
    for (int i = 0; i < 6; i++)
        settle(dv);
    drive(dv, framed, control_ids[ct]);
    if (framed) {
        /* and the embedding document goes on taking input */
        int x, y;
        if (centre(dv, false, "outer", &x, &y)) {
            mouse(dv, AU_EVENT_MOUSE_DOWN, x, y, 1, 0);
            mouse(dv, AU_EVENT_MOUSE_UP, x, y, 1, 0);
            key(dv, 0, 'c', 0);
        }
        settle(dv);
    }
    au_docview_free(dv);
    au_buf_free(&html);
    cases++;
}

int main(int argc, char **argv)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    /* AURA_TEST_GC_STRESS: collect whenever script is left, so an element
     * that only garbage in a reference cycle still names is freed when a
     * listener removes it (the collector seldom runs by itself on a small
     * page). It makes a case seven times as slow. */
    au_js_set_gc_stress(getenv("AURA_TEST_GC_STRESS") != NULL);
    int e0 = 0, e1 = NEVENTS, d0 = 0, d1 = NDESTROYERS, c0 = 0, c1 = NCONTROLS;
    /* Without arguments, a part of the matrix that ctest can afford:
     * three destroyers with every event and control, and the other
     * destroyers with every event and one control each. */
    bool quick = argc < 2;
    int a = 1;
    if (argc > 1 && !strcmp(argv[1], "all"))
        a = 2;
    if (argc > a) {
        e0 = atoi(argv[a]);
        e1 = e0 + 1;
    }
    if (argc > a + 1) {
        d0 = atoi(argv[a + 1]);
        d1 = d0 + 1;
    }
    if (argc > a + 2) {
        c0 = atoi(argv[a + 2]);
        c1 = c0 + 1;
    }
    if (e0 < 0 || e1 > NEVENTS || d0 < 0 || d1 > NDESTROYERS || c0 < 0 || c1 > NCONTROLS) {
        printf("usage: test_input_lifetime [all] [EVENT<%d [DESTROYER<%d [CONTROL<%d]]]\n", NEVENTS, NDESTROYERS,
               NCONTROLS);
        return 2;
    }
    bool verbose = getenv("AURA_TEST_VERBOSE") != NULL;
    for (int ev = e0; ev < e1; ev++)
        for (int ds = d0; ds < d1; ds++)
            for (int ct = c0; ct < c1; ct++)
                for (int framed = 0; framed < 2; framed++) {
                    if (quick) {
                        bool wide = ds == QUICK_DS_A || ds == QUICK_DS_B || ds == QUICK_DS_C;
                        if (wide ? framed != ((ev + ct) & 1)
                                 : ct != (ev * 5 + ds * 3) % NCONTROLS || framed != ((ev + ds) & 1))
                            continue;
                    }
                    if (verbose) {
                        printf("%d %d %d %s: %s / %s / #%s\n", ev, ds, ct, framed ? "framed" : "top", event_types[ev],
                               destroyers[ds], control_ids[ct]);
                        fflush(stdout);
                    }
                    run_case(ev, ds, ct, framed != 0);
                }
    printf("input lifetime: %d cases (%d submissions, %d navigations)\n", cases, submits, opens);
    au_fontset_free(fonts);
    return 0;
}
