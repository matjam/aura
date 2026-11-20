/* Form control tests: the editing model (src/page/editing.c), entry
 * lists and their encodings (formdata.c, against the HTML Standard's
 * algorithms), and through the document view: focus order, typing,
 * radio groups, selects, GET and POST submission, validation, and the
 * order of script events. */
#include "../layout/ahem.h"
#include "page/editing.h"
#include "page/formdata.h"
#include "layout/selection.h"
#include "page/page.h"
#include "page/docview_int.h" /* au_dv_edit_caret_rect */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures, checks;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);                                                            \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

static AuFontSet *fonts;

/* ---- Editing model ---- */

static AuElement *loose_input(AuDocument **doc_out)
{
    AuDocument *doc = au_document_new();
    AuNode *n = au_element_new(doc, AU_ATOM_input, AU_NS_HTML);
    au_node_append(&doc->node, n);
    au_node_unref(n);
    *doc_out = doc;
    return au_as_element(n);
}

static const char *val(AuElement *el)
{
    size_t n;
    return au_ctl_value(el, &n);
}

static void test_editing(void)
{
    AuDocument *doc;
    AuElement *el = loose_input(&doc);
    AuControl *c = au_ctl(el);
    au_edit_insert(c, "hello world", 11, -1, true, AU_EDIT_TYPING);
    CHECK(!strcmp(val(el), "hello world") && c->caret == 11, "insert: %s caret %u", val(el), (unsigned)c->caret);
    au_edit_move(c, -1, true, false); /* Option-Left: start of "world" */
    CHECK(c->caret == 6, "word left: %u", (unsigned)c->caret);
    au_edit_move(c, -1, true, true); /* extend to the start */
    uint32_t a, b;
    au_edit_range(c, &a, &b);
    CHECK(a == 0 && b == 6, "extend: %u..%u", (unsigned)a, (unsigned)b);
    AuBuf t;
    au_buf_init(&t);
    au_edit_selected_text(c, &t);
    CHECK(!strcmp(au_buf_cstr(&t), "hello "), "selected: %s", au_buf_cstr(&t));
    au_buf_free(&t);
    au_edit_insert(c, "X", 1, -1, true, AU_EDIT_TYPING); /* replaces the selection */
    CHECK(!strcmp(val(el), "Xworld"), "replace: %s", val(el));
    au_edit_undo(c);
    CHECK(!strcmp(val(el), "hello world"), "undo: %s", val(el));
    au_edit_undo(c); /* redo */
    CHECK(!strcmp(val(el), "Xworld"), "redo: %s", val(el));
    au_edit_move_to(c, 6, false);
    au_edit_delete(c, -1, true); /* Option-Backspace */
    CHECK(!strcmp(val(el), ""), "delete word back: '%s'", val(el));
    /* multi-byte characters move as one; newlines become spaces in one line */
    au_edit_insert(c, "a\xC3\xA9\n\xF0\x9F\x98\x80", 9, -1, true, AU_EDIT_OTHER);
    CHECK(!strcmp(val(el), "a\xC3\xA9 \xF0\x9F\x98\x80"), "utf-8 insert: %s", val(el));
    au_edit_delete(c, -1, false);
    CHECK(!strcmp(val(el), "a\xC3\xA9 "), "delete emoji: %s", val(el));
    au_edit_move(c, -1, false, false);
    au_edit_move(c, -1, false, false);
    CHECK(c->caret == 1, "move over e-acute: %u", (unsigned)c->caret);
    /* maxlength counts characters */
    au_edit_select_all(c);
    au_edit_insert(c, "\xC3\xA9\xC3\xA9\xC3\xA9xyz", 9, 4, true, AU_EDIT_OTHER);
    CHECK(!strcmp(val(el), "\xC3\xA9\xC3\xA9\xC3\xA9x"), "maxlength: %s", val(el));
    /* boundaries */
    const char *s = "one two\nthree";
    au_edit_word_at(s, strlen(s), 5, &a, &b);
    CHECK(a == 4 && b == 7, "word at: %u..%u", (unsigned)a, (unsigned)b);
    au_edit_line_at(s, strlen(s), 10, &a, &b);
    CHECK(a == 8 && b == 13, "line at: %u..%u", (unsigned)a, (unsigned)b);
    au_node_unref(&doc->node);
}

/* ---- Entry lists and encodings ---- */

static void test_encodings(void)
{
    AuFormEntries l;
    au_form_entries_init(&l);
    au_form_entries_add(&l, "a b", "c&d=e", 5);
    au_form_entries_add(&l, "\xC3\xA9", "x\ny\rz", 5);
    au_form_entries_add(&l, "k", "*-._~!", 6);
    AuBuf out;
    au_buf_init(&out);
    au_form_urlencode(&l, &out);
    CHECK(!strcmp(au_buf_cstr(&out), "a+b=c%26d%3De&%C3%A9=x%0D%0Ay%0D%0Az&k=*-._%7E%21"), "urlencoded: %s",
          au_buf_cstr(&out));
    au_buf_clear(&out);
    au_form_text_plain(&l, &out);
    CHECK(!strcmp(au_buf_cstr(&out), "a b=c&d=e\r\n\xC3\xA9=x\r\ny\r\nz\r\nk=*-._~!\r\n"), "text/plain: %s",
          au_buf_cstr(&out));
    au_form_entries_free(&l);

    au_form_entries_init(&l);
    au_form_entries_add(&l, "na\"me\n", "v\nw", 3);
    au_form_entries_add_file(&l, "f", "a.txt", "text/plain", "DATA", 4);
    au_buf_clear(&out);
    au_form_multipart(&l, "BOUND", &out);
    const char *want = "--BOUND\r\nContent-Disposition: form-data; name=\"na%22me%0D%0A\"\r\n\r\nv\r\nw\r\n"
                       "--BOUND\r\nContent-Disposition: form-data; name=\"f\"; filename=\"a.txt\"\r\n"
                       "Content-Type: text/plain\r\n\r\nDATA\r\n--BOUND--\r\n";
    CHECK(out.len == strlen(want) && !memcmp(out.data, want, out.len), "multipart:\n%s", au_buf_cstr(&out));
    au_form_entries_free(&l);
    au_buf_free(&out);
}

/* ---- Through the document view ---- */

typedef struct Ctx {
    char url[1024];
    char method[8];
    char body[1024];
    char type[128];
    int submits;
    char message[256];
    int invals;     /* invalidations since it was cleared */
    AuIRect inval;  /* their union */
} Ctx;

static void cb_invalidate(void *ctx, AuDocView *dv, AuIRect r)
{
    Ctx *c = ctx;
    (void)dv;
    if (!c->invals++) {
        c->inval = r;
        return;
    }
    if (r.x0 < c->inval.x0) c->inval.x0 = r.x0;
    if (r.y0 < c->inval.y0) c->inval.y0 = r.y0;
    if (r.x1 > c->inval.x1) c->inval.x1 = r.x1;
    if (r.y1 > c->inval.y1) c->inval.y1 = r.y1;
}

static void cb_submit(void *ctx, AuDocView *dv, const AuFormSubmission *s)
{
    Ctx *c = ctx;
    (void)dv;
    c->submits++;
    snprintf(c->url, sizeof c->url, "%s", s->url);
    snprintf(c->method, sizeof c->method, "%s", s->method);
    snprintf(c->body, sizeof c->body, "%.*s", (int)s->body_len, s->body ? (const char *)s->body : "");
    snprintf(c->type, sizeof c->type, "%s", s->body_type ? s->body_type : "");
}

static void cb_message(void *ctx, AuDocView *dv, const char *text)
{
    Ctx *c = ctx;
    (void)dv;
    snprintf(c->message, sizeof c->message, "%s", text);
}

static AuDocView *view(Ctx *c, const char *html, bool script)
{
    memset(c, 0, sizeof *c);
    AuDocViewCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.ctx = c;
    cb.submit = cb_submit;
    cb.message = cb_message;
    cb.invalidate = cb_invalidate;
    AuDocView *dv = au_docview_new(fonts, NULL, &cb);
    au_docview_set_scripting(dv, script);
    au_docview_set_viewport(dv, 800, 600);
    au_docview_load(dv, html, strlen(html), "http://example.com/dir/page.html");
    for (int i = 0; i < 20; i++)
        au_docview_run_scripts(dv, 10);
    au_docview_update(dv);
    return dv;
}

static AuElement *by_id(AuDocView *dv, const char *id)
{
    return au_docview_find_fragment(dv, id);
}

static const char *focus_id(AuDocView *dv)
{
    AuElement *f = au_docview_focus(dv);
    if (!f)
        return "(none)";
    const char *id = au_element_get_attr(f, AU_ATOM_id);
    return id ? id : "(no id)";
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

static void type(AuDocView *dv, const char *s)
{
    for (; *s; s++)
        key(dv, 0, (unsigned char)*s, 0);
}

static void click(AuDocView *dv, const char *id)
{
    AuRect r;
    au_docview_update(dv);
    if (!au_layout_element_rect(au_docview_layout(dv), by_id(dv, id), &r)) {
        CHECK(0, "no box for #%s", id);
        return;
    }
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.x = (int)(r.x + r.w / 2);
    e.y = (int)(r.y + r.h / 2);
    e.clicks = 1;
    e.type = AU_EVENT_MOUSE_DOWN;
    au_docview_mouse(dv, &e);
    e.type = AU_EVENT_MOUSE_UP;
    au_docview_mouse(dv, &e);
}

static void test_focus_order(void)
{
    Ctx c;
    AuDocView *dv = view(&c,
                         "<a id=l href=#x>link</a> <input id=a tabindex=2> <input id=b> <input id=c tabindex=1>"
                         " <input id=d disabled> <input id=e tabindex=-1> <input id=h type=hidden> <button id=f>f</button>",
                         false);
    const char *order[] = {"c", "a", "l", "b", "f", "c"};
    for (int i = 0; i < 6; i++) {
        key(dv, AU_KEY_TAB, 0, 0);
        CHECK(!strcmp(focus_id(dv), order[i]), "tab %d: %s, want %s", i, focus_id(dv), order[i]);
    }
    key(dv, AU_KEY_TAB, 0, AU_MOD_SHIFT);
    CHECK(!strcmp(focus_id(dv), "f"), "shift-tab: %s", focus_id(dv));
    click(dv, "e"); /* tabindex -1 is focusable by click */
    CHECK(!strcmp(focus_id(dv), "e"), "click focus: %s", focus_id(dv));
    CHECK(au_docview_editing(dv), "editing after click");
    au_docview_free(dv);

    dv = view(&c, "<input id=a><input id=b autofocus>", false);
    CHECK(!strcmp(focus_id(dv), "b"), "autofocus: %s", focus_id(dv));
    au_docview_free(dv);
}

static void test_typing_and_get(void)
{
    Ctx c;
    AuDocView *dv = view(&c,
                         "<form action=\"search#top\"><input id=q name=q value=old><input type=hidden name=h value=1>"
                         "<input type=checkbox name=cb id=cb checked><input id=s type=submit name=go value=Go></form>",
                         false);
    click(dv, "q");
    key(dv, 0, 'a', AU_MOD_COMMAND); /* select all */
    type(dv, "mac os");
    key(dv, AU_KEY_BACKSPACE, 0, 0);
    type(dv, "s 9");
    size_t n;
    CHECK(!strcmp(au_ctl_value(by_id(dv, "q"), &n), "mac os 9"), "typed: %s", au_ctl_value(by_id(dv, "q"), &n));
    key(dv, AU_KEY_RETURN, 0, 0);
    CHECK(c.submits == 1, "implicit submission: %d", c.submits);
    CHECK(!strcmp(c.url, "http://example.com/dir/search?q=mac+os+9&h=1&cb=on&go=Go#top"), "GET url: %s", c.url);
    CHECK(!strcmp(c.method, "GET"), "method %s", c.method);
    /* clicking the checkbox unchecks it; the button submits */
    click(dv, "cb");
    click(dv, "s");
    CHECK(c.submits == 2 && !strcmp(c.url, "http://example.com/dir/search?q=mac+os+9&h=1&go=Go#top"), "click submit: %s",
          c.url);
    au_docview_free(dv);
}

static void test_post_and_multipart(void)
{
    Ctx c;
    AuDocView *dv = view(&c,
                         "<form method=post action=/post><textarea id=t name=t></textarea>"
                         "<select name=s id=s><option>one<option value=2>two</select>"
                         "<button id=b formenctype=multipart/form-data name=b value=x>B</button></form>",
                         false);
    click(dv, "t");
    type(dv, "line1");
    key(dv, AU_KEY_RETURN, 0, 0); /* a newline in a textarea, no submission */
    type(dv, "line2");
    CHECK(c.submits == 0, "return in a textarea submitted");
    click(dv, "s");
    key(dv, AU_KEY_DOWN, 0, 0); /* no pop-up callback: Down chooses the next option */
    au_docview_submit(dv, au_form_owner(by_id(dv, "t")), NULL);
    CHECK(c.submits == 1 && !strcmp(c.method, "POST"), "POST: %d %s", c.submits, c.method);
    CHECK(!strcmp(c.url, "http://example.com/post"), "POST url %s", c.url);
    CHECK(!strcmp(c.body, "t=line1%0D%0Aline2&s=2"), "POST body %s", c.body);
    CHECK(!strcmp(c.type, "application/x-www-form-urlencoded"), "POST type %s", c.type);
    click(dv, "b");
    CHECK(c.submits == 2 && !strncmp(c.type, "multipart/form-data; boundary=", 30), "multipart type %s", c.type);
    CHECK(strstr(c.body, "name=\"t\"\r\n\r\nline1\r\nline2\r\n") && strstr(c.body, "name=\"b\"\r\n\r\nx\r\n"),
          "multipart body %s", c.body);
    au_docview_free(dv);
}

static void test_radio_and_select(void)
{
    Ctx c;
    AuDocView *dv = view(&c,
                         "<form><input type=radio name=r id=r1 value=1 checked><input type=radio name=r id=r2 value=2>"
                         "<input type=radio name=r id=r3 value=3 disabled><input type=radio name=other id=o>"
                         "<select id=s><option>a<option disabled>b<option>c</select>"
                         "<select id=m multiple size=3><option>x<option>y<option>z</select></form>",
                         false);
    click(dv, "r2");
    CHECK(!au_ctl_checked(by_id(dv, "r1")) && au_ctl_checked(by_id(dv, "r2")), "radio click");
    click(dv, "o");
    CHECK(au_ctl_checked(by_id(dv, "r2")) && au_ctl_checked(by_id(dv, "o")), "other group unaffected");
    click(dv, "r2");
    key(dv, AU_KEY_RIGHT, 0, 0); /* r3 is disabled: wraps to r1 */
    CHECK(au_ctl_checked(by_id(dv, "r1")) && !strcmp(focus_id(dv), "r1"), "radio arrow: focus %s", focus_id(dv));
    click(dv, "s");
    key(dv, AU_KEY_DOWN, 0, 0); /* skips the disabled option */
    CHECK(au_select_index(by_id(dv, "s")) == 2, "select down: %d", au_select_index(by_id(dv, "s")));
    click(dv, "m");
    key(dv, AU_KEY_DOWN, 0, AU_MOD_SHIFT);
    size_t n;
    AuElement **o = au_select_options(by_id(dv, "m"), false, &n);
    int sel = 0;
    for (size_t i = 0; i < n; i++)
        sel += au_ctl_checked(o[i]) ? 1 << i : 0;
    au_free(o);
    CHECK(sel == 3 || sel == 6, "list box shift-down: mask %d", sel);
    au_docview_free(dv);
}

static void test_validation(void)
{
    Ctx c;
    AuDocView *dv = view(&c,
                         "<form action=/x><input id=a name=a><input id=r name=r required>"
                         "<input id=e name=e type=email value=bad><input id=go type=submit></form>",
                         false);
    click(dv, "a");
    key(dv, AU_KEY_RETURN, 0, 0);
    CHECK(c.submits == 0, "submitted with a missing required value");
    CHECK(!strcmp(focus_id(dv), "r"), "focus on the invalid control: %s", focus_id(dv));
    CHECK(c.message[0] != 0, "no validation message");
    type(dv, "v");
    click(dv, "go");
    CHECK(c.submits == 0 && !strcmp(focus_id(dv), "e"), "email check: %d, focus %s", c.submits, focus_id(dv));
    key(dv, 0, 'a', AU_MOD_COMMAND);
    type(dv, "a@b.c");
    click(dv, "go");
    CHECK(c.submits == 1, "valid form not submitted");
    au_docview_free(dv);

    dv = view(&c, "<form action=/x novalidate><input id=r required><input id=go type=submit></form>", false);
    click(dv, "go");
    CHECK(c.submits == 1, "novalidate");
    au_docview_free(dv);
}

/* Google's search box, reduced: the keydown listener cancels Enter, then
 * defers the submission through window.postMessage with location.href as
 * the target origin (only its origin counts) and calls form.submit() from
 * the message listener. Enter also gets a keypress with code 13. */
static void test_search_box_defer(void)
{
    Ctx c;
    AuDocView *dv = view(&c,
                         "<form id=f action=/search><input id=q name=q><input type=submit name=btnG value=Go></form>"
                         "<script>var f=document.getElementById('f'),q=document.getElementById('q'),queue=[],log=[];"
                         "window.addEventListener('message',function(e){"
                         "if(e.source===window&&e.data==='sbox.df'&&queue.length)queue.shift()();},false);"
                         "function defer(fn){queue.push(fn);window.postMessage('sbox.df',window.location.href);}"
                         "q.addEventListener('keypress',function(e){log.push('keypress:'+e.keyCode+':'+e.charCode+':'+e.which);});"
                         "q.addEventListener('keydown',function(e){if(e.keyCode==13){e.stopPropagation();"
                         "e.preventDefault();e.returnValue=false;defer(function(){f.submit();});}});"
                         "</script>",
                         true);
    click(dv, "q");
    type(dv, "mac");
    key(dv, AU_KEY_RETURN, 0, 0);
    for (int i = 0; i < 20; i++)
        au_docview_run_scripts(dv, 10);
    CHECK(c.submits == 1, "deferred submit through postMessage(data, location.href): %d submits", c.submits);
    au_docview_free(dv);

    dv = view(&c,
              "<form id=f action=/s><input id=q name=q></form><script>var log=[];"
              "document.getElementById('q').addEventListener('keypress',function(e){"
              "log.push(e.key+':'+e.keyCode+':'+e.charCode+':'+e.which);});"
              "document.getElementById('f').addEventListener('submit',function(e){log.push('submit');e.preventDefault();});"
              "</script>",
              true);
    click(dv, "q");
    type(dv, "a");
    key(dv, AU_KEY_RETURN, 0, 0);
    AuBuf out;
    au_buf_init(&out);
    au_docview_eval(dv, "log.join(' ')", &out);
    CHECK(!strcmp(au_buf_cstr(&out), "a:97:97:97 Enter:13:13:13 submit"), "keypress for Enter: %s", au_buf_cstr(&out));
    au_buf_clear(&out);
    au_docview_free(dv);

    /* a cancelled Enter keypress stops implicit submission */
    dv = view(&c,
              "<form id=f action=/s><input id=q name=q></form><script>"
              "document.getElementById('q').addEventListener('keypress',function(e){e.preventDefault();});</script>",
              true);
    click(dv, "q");
    key(dv, AU_KEY_RETURN, 0, 0);
    CHECK(c.submits == 0, "cancelled keypress submits");
    au_buf_free(&out);
    au_docview_free(dv);
}

/* What a keystroke in a text field costs. Once the field is not empty, a
 * character typed or deleted restyles nothing, builds no box tree, runs no
 * layout and invalidates the field's rectangle only, on a page with
 * :has() rules (where a restyle covers the whole document), with and
 * without script listeners. The first character (the field stops being
 * empty: :placeholder-shown) may restyle. */
static void keystroke_costs(const char *what, bool script, const char *extra, unsigned may_style)
{
    Ctx c;
    AuBuf html;
    au_buf_init(&html);
    au_buf_puts(&html, "<style>section:has(input:focus){background:#eee} p:has(+ p){margin:2px}"
                       " input:placeholder-shown{color:gray} input:invalid{outline:1px solid red}</style>"
                       "<section><form action=/s><input id=q name=q placeholder=Search required>"
                       "<input type=hidden id=h name=h></form></section>");
    for (int i = 0; i < 200; i++)
        au_buf_puts(&html, "<p>Paragraph <b>bold</b> <i>italic</i> text</p>");
    au_buf_puts(&html, extra);
    AuDocView *dv = view(&c, au_buf_cstr(&html), script);
    au_buf_free(&html);
    click(dv, "q");
    type(dv, "a");
    for (int i = 0; i < 5; i++)
        au_docview_run_scripts(dv, 10);
    au_docview_update(dv);
    AuRect fr;
    CHECK(au_layout_element_rect(au_docview_layout(dv), by_id(dv, "q"), &fr), "%s: the field has a box", what);
    static const struct {
        const char *text;
        int key;
    } keys[] = {{"b", 0}, {"c", 0}, {" ", 0}, {NULL, AU_KEY_BACKSPACE}, {"d", 0}, {NULL, AU_KEY_LEFT}, {"e", 0}};
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        AuDocViewStats s0, s1;
        au_docview_stats(dv, &s0);
        c.invals = 0;
        if (keys[k].text)
            type(dv, keys[k].text);
        else
            key(dv, keys[k].key, 0, 0);
        for (int i = 0; i < 5; i++)
            au_docview_run_scripts(dv, 10);
        au_docview_update(dv);
        au_docview_stats(dv, &s1);
        /* may_style: elements a listener's attribute changes may restyle */
        CHECK((may_style || s1.style_passes == s0.style_passes) && s1.styled_elements - s0.styled_elements <= may_style,
              "%s, key %d: %u style passes, %u elements styled", what, (int)k, s1.style_passes - s0.style_passes,
              s1.styled_elements - s0.styled_elements);
        CHECK(s1.tree_builds == s0.tree_builds && s1.layouts == s0.layouts, "%s, key %d: %u tree builds, %u layouts",
              what, (int)k, s1.tree_builds - s0.tree_builds, s1.layouts - s0.layouts);
        CHECK(c.invals >= 1 && c.inval.x0 >= (int)fr.x - 5 && c.inval.y0 >= (int)fr.y - 5 &&
                  c.inval.x1 <= (int)(fr.x + fr.w) + 6 && c.inval.y1 <= (int)(fr.y + fr.h) + 6,
              "%s, key %d: invalidated %d,%d-%d,%d, field %d,%d-%d,%d", what, (int)k, c.inval.x0, c.inval.y0,
              c.inval.x1, c.inval.y1, (int)fr.x, (int)fr.y, (int)(fr.x + fr.w), (int)(fr.y + fr.h));
    }
    size_t n;
    CHECK(!strcmp(au_ctl_value(by_id(dv, "q"), &n), "abced"), "%s: value %s", what, au_ctl_value(by_id(dv, "q"), &n));
    au_docview_free(dv);
}

static void test_keystroke_cost(void)
{
    keystroke_costs("no script", false, "", 0);
    /* a listener that changes attributes in the document on every key, as
     * Wikipedia's search field does (a hidden input's value, an aria
     * attribute): at most those elements restyle, nothing is laid out and
     * only the field is repainted */
    keystroke_costs("attribute writers", true,
                    "<script>var q=document.getElementById('q'),h=document.getElementById('h');"
                    "q.addEventListener('input',function(){h.value=q.value;h.setAttribute('data-n',q.value.length);"
                    "q.setAttribute('aria-expanded',q.value.length%2?'true':'false');});</script>",
                    4);
    /* listeners that read the field and build a detached tree, as a
     * suggestions widget does before it shows anything */
    keystroke_costs("listeners", true,
                    "<script>var q=document.getElementById('q'),seen=0,built;"
                    "['keydown','keypress','beforeinput','input','keyup'].forEach(function(t){"
                    "q.addEventListener(t,function(e){seen+=q.value.length;});});"
                    "q.addEventListener('input',function(){built=document.createElement('ul');"
                    "for(var i=0;i<5;i++){var li=document.createElement('li');li.className='item';"
                    "li.setAttribute('role','option');li.textContent=q.value+i;built.appendChild(li);}});"
                    "</script>",
                    0);
}

static void test_script_events(void)
{
    Ctx c;
    AuDocView *dv = view(&c,
                         "<form id=f action=/s><input id=q name=q></form><input id=n><script>"
                         "var log=[];var q=document.getElementById('q');"
                         "['keydown','beforeinput','input','keyup','change','focus','blur'].forEach(function(t){"
                         "q.addEventListener(t,function(e){log.push(t+(e.key?':'+e.key:''));"
                         "if(t==='keydown'&&e.key==='x')e.preventDefault();"
                         "if(t==='beforeinput'&&e.data==='y')e.preventDefault();});});"
                         "document.getElementById('f').addEventListener('submit',function(e){"
                         "log.push('submit:'+(e.submitter?e.submitter.id:'none'));e.preventDefault();});"
                         "</script>",
                         true);
    click(dv, "q");
    type(dv, "axy");
    key(dv, AU_KEY_RETURN, 0, 0);
    click(dv, "n");
    AuBuf out;
    au_buf_init(&out);
    au_docview_eval(dv, "log.join(' ')+' | '+q.value", &out);
    const char *want = "focus keydown:a beforeinput input keyup:a keydown:x keyup:x keydown:y beforeinput keyup:y "
                       "keydown:Enter change submit:none keyup:Enter blur | a";
    CHECK(!strcmp(au_buf_cstr(&out), want), "event order:\n  %s\nwant\n  %s", au_buf_cstr(&out), want);
    CHECK(c.submits == 0, "submit not cancelled");
    /* script replaces the value while focused; editing goes on */
    au_buf_clear(&out);
    click(dv, "q");
    au_docview_eval(dv, "q.value='zz'; q.setSelectionRange(1,1); ''", &out);
    type(dv, "a");
    size_t n;
    CHECK(!strcmp(au_ctl_value(by_id(dv, "q"), &n), "zaz"), "after script value: %s", au_ctl_value(by_id(dv, "q"), &n));
    au_buf_free(&out);
    au_docview_free(dv);

    /* a throwing handler does not stop typing */
    dv = view(&c, "<input id=q><script>document.getElementById('q').oninput=function(){throw new Error('x')}</script>",
              true);
    click(dv, "q");
    type(dv, "ok");
    CHECK(!strcmp(au_ctl_value(by_id(dv, "q"), &n), "ok"), "typing with a throwing handler: %s",
          au_ctl_value(by_id(dv, "q"), &n));
    au_docview_free(dv);
}

static void click_at(AuDocView *dv, int x, int y)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.x = x;
    e.y = y;
    e.clicks = 1;
    e.type = AU_EVENT_MOUSE_DOWN;
    au_docview_mouse(dv, &e);
    e.type = AU_EVENT_MOUSE_UP;
    au_docview_mouse(dv, &e);
}

static const char *eval_str(AuDocView *dv, const char *js, AuBuf *out)
{
    au_buf_clear(out);
    au_docview_eval(dv, js, out);
    return au_buf_cstr(out);
}

static void mouse_at(AuDocView *dv, AuEventType t, int x, int y)
{
    AuEvent e;
    memset(&e, 0, sizeof e);
    e.x = x;
    e.y = y;
    e.clicks = 1;
    e.type = t;
    au_docview_mouse(dv, &e);
}

/* A scroller that is a flex item keeps its offset through relayout: the
 * flex container measures it at its content height first, which clamped
 * the offset to 0 for good (Discord's message list jumped up). */
static void test_flex_scroller_keeps_offset(void)
{
    Ctx c;
    AuBuf out;
    au_buf_init(&out);
    AuDocView *dv = view(&c,
                         "<style>body{margin:0} #o div{height:40px}</style>"
                         "<div style='display:flex;flex-direction:column;height:400px'>"
                         "<div id=s style='flex:1 1 auto;min-height:0;overflow-y:scroll'><div id=o></div></div>"
                         "<div style='flex:0 0 50px'></div></div>"
                         "<script>var o=document.getElementById('o'),s=document.getElementById('s');"
                         "for(var i=0;i<60;i++)o.appendChild(document.createElement('div'))</script>",
                         true);
    CHECK(!strcmp(eval_str(dv, "s.scrollTop=1500;s.scrollTop+':'+s.scrollHeight", &out), "1500:2400"),
          "scrolled: %s", au_buf_cstr(&out));
    CHECK(!strcmp(eval_str(dv, "o.firstChild.textContent='x';String(s.scrollTop)", &out), "1500"),
          "a change inside keeps the offset: %s", au_buf_cstr(&out));
    CHECK(!strcmp(eval_str(dv, "o.appendChild(document.createElement('div'));s.scrollTop+':'+s.scrollHeight", &out),
                  "1500:2440"),
          "content added keeps it: %s", au_buf_cstr(&out));
    /* A real shrink still clamps, and stays clamped when the content grows. */
    CHECK(!strcmp(eval_str(dv, "while(o.children.length>20)o.lastChild.remove();String(s.scrollTop)", &out), "450"),
          "a shrink clamps: %s", au_buf_cstr(&out));
    CHECK(!strcmp(eval_str(dv, "for(var i=0;i<40;i++)o.appendChild(document.createElement('div'));"
                               "String(s.scrollTop)",
                           &out),
                  "450"),
          "growing again does not return to the old offset: %s", au_buf_cstr(&out));
    au_buf_free(&out);
    au_docview_free(dv);
}

/* contenteditable: a press puts the caret at the text pressed, keys edit
 * with beforeinput and input around each change, a cancelled beforeinput
 * or keypress leaves the text alone (Discord's Slate editor edits it
 * itself), and the view has a caret to draw. */
static void test_contenteditable(void)
{
    Ctx c;
    AuBuf out;
    au_buf_init(&out);
    AuDocView *dv = view(&c,
                         "<style>body{margin:0;font:16px sans-serif}</style>"
                         "<div id=e contenteditable style='width:300px;height:40px;padding:4px'>hello world</div>"
                         "<p id=p>not editable</p>"
                         "<script>var log=[],e=document.getElementById('e'),block='';"
                         "['beforeinput','input'].forEach(function(t){e.addEventListener(t,function(ev){"
                         "log.push(t+':'+ev.inputType+(ev.data?':'+ev.data:''));"
                         "if(t=='beforeinput'&&ev.data===block)ev.preventDefault()})});"
                         "e.addEventListener('keypress',function(ev){if(ev.key=='!')ev.preventDefault()});"
                         "function st(){var s=getSelection();return e.innerHTML+'|'+(s.anchorNode&&s.anchorNode.nodeName)+"
                         "':'+s.anchorOffset+'|'+(document.activeElement===e)}</script>",
                         true);
    CHECK(!au_docview_editing(dv), "nothing to edit yet");
    /* a press after "hello" (about 38 px of 16 px text) */
    mouse_at(dv, AU_EVENT_MOUSE_DOWN, 300, 12);
    mouse_at(dv, AU_EVENT_MOUSE_UP, 300, 12);
    CHECK(!strcmp(eval_str(dv, "st()", &out), "hello world|#text:11|true"), "a press past the text: %s",
          au_buf_cstr(&out));
    CHECK(au_docview_editing(dv), "the view has a caret");
    AuRect r;
    uint32_t col;
    CHECK(au_dv_edit_caret_rect(dv, &r, &col) && r.w == 1 && r.h > 10 && r.x > 50, "caret rect %g,%g %gx%g", r.x,
          r.y, r.w, r.h);
    type(dv, "!?");
    CHECK(!strcmp(eval_str(dv, "st()+' '+log.join(',')", &out),
                  "hello world?|#text:12|true beforeinput:insertText:?,input:insertText:?"),
          "typing: a cancelled keypress types nothing: %s", au_buf_cstr(&out));
    key(dv, AU_KEY_BACKSPACE, 0, 0);
    key(dv, AU_KEY_LEFT, 0, 0);
    key(dv, AU_KEY_LEFT, 0, 0);
    eval_str(dv, "log=[];block='x'", &out);
    type(dv, "xY");
    CHECK(!strcmp(eval_str(dv, "st()+' '+log.join(',')", &out),
                  "hello worYld|#text:10|true beforeinput:insertText:x,beforeinput:insertText:Y,input:insertText:Y"),
          "Backspace, arrows, a cancelled beforeinput: %s", au_buf_cstr(&out));
    key(dv, AU_KEY_END, 0, 0);
    key(dv, AU_KEY_RETURN, 0, 0);
    type(dv, "2");
    CHECK(!strcmp(eval_str(dv, "e.innerHTML", &out), "hello worYld<br>2<br>"), "Enter: %s", au_buf_cstr(&out));
    /* A press outside takes the focus and the caret away. */
    mouse_at(dv, AU_EVENT_MOUSE_DOWN, 300, 200);
    CHECK(!au_docview_editing(dv) && !au_dv_edit_caret_rect(dv, &r, &col), "no caret after a press outside");
    /* Script focus keeps a Selection already in the host, and puts one
     * that is elsewhere at the host's start. */
    CHECK(!strcmp(eval_str(dv, "e.focus();st()", &out), "hello worYld<br>2<br>|#text:1|true"), "focus(): %s",
          au_buf_cstr(&out));
    CHECK(!strcmp(eval_str(dv, "e.blur();getSelection().collapse(document.getElementById('p'),0);e.focus();st()", &out),
                  "hello worYld<br>2<br>|#text:0|true"),
          "focus() with the Selection elsewhere: %s", au_buf_cstr(&out));
    au_buf_free(&out);
    au_docview_free(dv);
}

/* The mouse as script sees it: pointer events before each mouse event,
 * offsetX/offsetY from the target's padding edge, and pointer capture
 * that sends a drag to the capturing element (hCaptcha's drag puzzles
 * read all three). */
/* The wheel over an inner scroll container scrolls it and fires scroll
 * at it, once per run, not bubbling; scrollTop then reads where it is.
 * Discord's message list keeps its place from these events. */
static void test_inner_scroll_event(void)
{
    Ctx c;
    AuBuf out;
    au_buf_init(&out);
    AuDocView *dv = view(&c,
                         "<style>body{margin:0}</style>"
                         "<div id=s style='height:100px;width:200px;overflow-y:scroll'><div style='height:1000px'></div></div>"
                         "<script>var n=0,top_=-1,bub=0;var s=document.getElementById('s');"
                         "s.addEventListener('scroll',function(e){n++;top_=s.scrollTop});"
                         "document.addEventListener('scroll',function(){bub++})</script>",
                         true);
    CHECK(au_docview_scroll_at(dv, 50, 50, 0, 40), "inner box scrolls");
    au_docview_scroll_at(dv, 50, 50, 0, 40);
    au_docview_run_scripts(dv, 10);
    CHECK(!strcmp(eval_str(dv, "n+':'+top_+':'+bub+':'+s.scrollTop", &out), "1:80:0:80"),
          "scroll event at the scrolled box: %s", au_buf_cstr(&out));
    /* Script scrolling fires it too, later, and only when the box moves. */
    CHECK(!strcmp(eval_str(dv, "n=0;s.scrollTop=500;s.scrollTop=600;String(n)", &out), "0"),
          "script scroll event is not synchronous: %s", au_buf_cstr(&out));
    au_docview_run_scripts(dv, 10);
    CHECK(!strcmp(eval_str(dv, "n+':'+top_", &out), "1:600"), "script scroll event: %s", au_buf_cstr(&out));
    eval_str(dv, "n=0;s.scrollTop=600", &out);
    au_docview_run_scripts(dv, 10);
    CHECK(!strcmp(eval_str(dv, "String(n)", &out), "0"), "no event when the box does not move: %s",
          au_buf_cstr(&out));
    au_buf_free(&out);
    au_docview_free(dv);
}

static void test_pointer_events(void)
{
    Ctx c;
    AuBuf out;
    au_buf_init(&out);
    AuDocView *dv = view(&c,
                         "<style>body{margin:0} div{position:absolute;width:100px;height:100px}</style>"
                         "<div id=a style='left:50px;top:40px;border:5px solid'></div>"
                         "<div id=b style='left:300px;top:40px'></div>"
                         "<script>var log=[];['pointerdown','pointermove','pointerup','mousedown','mousemove','mouseup',"
                         "'gotpointercapture','lostpointercapture'].forEach(function(t){document.addEventListener(t,"
                         "function(e){log.push(e.type.replace('pointer','p').replace('mouse','m').replace('gotpcapture',"
                         "'got').replace('lostpcapture','lost')+':'+e.target.id+':'+e.offsetX+','+e.offsetY+':'+e.button+"
                         "'/'+e.buttons+(e.pointerType?':'+e.pointerType+':'+e.isTrusted:''))},true)});"
                         "</script>",
                         true);
    /* a press and release inside a: offsets from a's padding edge (55,45) */
    mouse_at(dv, AU_EVENT_MOUSE_MOVE, 75, 65);
    mouse_at(dv, AU_EVENT_MOUSE_DOWN, 75, 65);
    mouse_at(dv, AU_EVENT_MOUSE_UP, 75, 65);
    CHECK(!strcmp(eval_str(dv, "log.join(' ')", &out),
                  "pmove:a:20,20:-1/0:mouse:true mmove:a:20,20:0/0 pdown:a:20,20:0/1:mouse:true mdown:a:20,20:0/1 "
                  "pup:a:20,20:0/0:mouse:true mup:a:20,20:0/0"),
          "pointer and mouse events: %s", au_buf_cstr(&out));
    /* a drag captured by a reaches a outside it, then capture is released */
    eval_str(dv, "log=[];document.getElementById('a').addEventListener('pointerdown',function(e){"
                 "this.setPointerCapture(e.pointerId)})", &out);
    mouse_at(dv, AU_EVENT_MOUSE_DOWN, 75, 65);
    mouse_at(dv, AU_EVENT_MOUSE_MOVE, 320, 60);
    mouse_at(dv, AU_EVENT_MOUSE_UP, 320, 60);
    CHECK(!strcmp(eval_str(dv, "log.join(' ')", &out),
                  "pdown:a:20,20:0/1:mouse:true mdown:a:20,20:0/1 got:a:265,15:-1/1:mouse:true "
                  "pmove:a:265,15:-1/1:mouse:true mmove:a:265,15:0/1 pup:a:265,15:0/0:mouse:true "
                  "lost:a:265,15:-1/0:mouse:true mup:a:265,15:0/0"),
          "captured drag: %s", au_buf_cstr(&out));
    CHECK(!strcmp(eval_str(dv, "document.getElementById('a').hasPointerCapture(1)", &out), "false"),
          "capture kept after the release: %s", au_buf_cstr(&out));
    /* a cancelled pointerdown suppresses the mouse events until the release */
    eval_str(dv, "log=[];document.addEventListener('pointerdown',function(e){e.preventDefault()})", &out);
    mouse_at(dv, AU_EVENT_MOUSE_DOWN, 320, 60);
    mouse_at(dv, AU_EVENT_MOUSE_MOVE, 330, 60);
    mouse_at(dv, AU_EVENT_MOUSE_UP, 330, 60);
    mouse_at(dv, AU_EVENT_MOUSE_MOVE, 331, 60);
    CHECK(!strcmp(eval_str(dv, "log.join(' ')", &out),
                  "pdown:b:20,20:0/1:mouse:true pmove:b:30,20:-1/1:mouse:true pup:b:30,20:0/0:mouse:true "
                  "pmove:b:31,20:-1/0:mouse:true mmove:b:31,20:0/0"),
          "cancelled pointerdown: %s", au_buf_cstr(&out));
    /* setPointerCapture: only the mouse (1), and only while a button is down */
    CHECK(!strcmp(eval_str(dv, "var n='';try{document.getElementById('a').setPointerCapture(7)}catch(e){n=e.name}n", &out),
                  "NotFoundError"),
          "capture of an unknown pointer: %s", au_buf_cstr(&out));
    CHECK(!strcmp(eval_str(dv, "var a=document.getElementById('a');a.setPointerCapture(1);a.hasPointerCapture(1)", &out),
                  "false"),
          "capture with no button down: %s", au_buf_cstr(&out));
    /* a script's MouseEvent has offsets from its clientX once dispatched */
    CHECK(!strcmp(eval_str(dv, "var o;a.addEventListener('x',function(e){o=e.offsetX+','+e.offsetY});"
                               "a.dispatchEvent(new MouseEvent('x',{clientX:60,clientY:50}));o", &out),
                  "5,5"),
          "a synthetic event's offsets: %s", au_buf_cstr(&out));
    au_docview_free(dv);
    au_buf_free(&out);
}

/* A modal dialog makes the rest of the document inert (HTML, "inert
 * subtrees"): clicks outside it reach no listener, Tab stays inside it,
 * Escape cancels and closes it, and Select All and find see only its
 * text. Escape closes an auto popover, and a click outside hides it. */
static void test_modal_dialog(void)
{
    Ctx c;
    AuBuf out;
    au_buf_init(&out);
    AuDocView *dv = view(&c,
                         "<style>body{margin:0} dialog::backdrop{display:none}"
                         " dialog{border:0;padding:0;width:200px;height:100px}</style>"
                         "<button id=out>outside text</button><input id=out2>"
                         "<dialog id=d><input id=in1><input id=in2> inside text</dialog>"
                         "<script>var log=[];"
                         "document.addEventListener('click',function(e){log.push('click:'+e.target.id)});"
                         "document.addEventListener('mousedown',function(e){log.push('down:'+e.target.id)});"
                         "var d=document.getElementById('d');"
                         "d.addEventListener('cancel',function(e){log.push('cancel');if(window.keep)e.preventDefault()});"
                         "d.addEventListener('close',function(){log.push('close')});"
                         "</script>",
                         true);
    click(dv, "out2");
    CHECK(!strcmp(focus_id(dv), "out2"), "focus before showModal: %s", focus_id(dv));
    eval_str(dv, "d.showModal(); log=[]", &out);
    au_docview_update(dv);
    CHECK(!strcmp(focus_id(dv), "in1"), "showModal focuses the dialog's first control: %s", focus_id(dv));
    /* outside the dialog (no backdrop): no target, focus stays */
    click_at(dv, 5, 5);
    click_at(dv, 5, 590);
    CHECK(!strcmp(eval_str(dv, "log.join(' ')", &out), ""), "clicks outside the dialog reached the page: %s",
          au_buf_cstr(&out));
    CHECK(!strcmp(focus_id(dv), "in1"), "a click outside moved the focus: %s", focus_id(dv));
    CHECK(!strcmp(eval_str(dv, "document.getElementById('out').focus(); document.activeElement.id", &out), "in1"),
          "focus() on inert content: %s", au_buf_cstr(&out));
    /* Tab cycles inside the dialog */
    const char *order[] = {"in2", "in1", "in2"};
    for (int i = 0; i < 3; i++) {
        key(dv, AU_KEY_TAB, 0, 0);
        CHECK(!strcmp(focus_id(dv), order[i]), "tab %d in the dialog: %s, want %s", i, focus_id(dv), order[i]);
    }
    /* a click inside reaches the page */
    click(dv, "in1");
    CHECK(!strcmp(eval_str(dv, "log.join(' ')", &out), "down:in1 click:in1"), "click inside: %s", au_buf_cstr(&out));
    /* Select All and find see only the dialog's text */
    AuLayout *L = au_docview_layout(dv);
    AuTextRange all, m;
    au_buf_clear(&out);
    if (au_layout_all_text(L, &all))
        au_layout_range_text(L, &all, &out);
    CHECK(!strcmp(au_buf_cstr(&out), " inside text"), "select all with a modal dialog: '%s'", au_buf_cstr(&out));
    CHECK(!au_layout_find(L, "outside", NULL, true, true, &m), "find matched inert text");
    CHECK(au_layout_find(L, "inside", NULL, true, true, &m), "find missed the dialog's text");
    AuTextPos q;
    CHECK(!au_layout_text_pos_at(L, 5, 590, 0, 0, &q), "a press outside the dialog has a text position");
    /* Escape: cancel, then close unless cancelled; focus goes back */
    eval_str(dv, "log=[]; window.keep=true", &out);
    key(dv, AU_KEY_ESCAPE, 0, 0);
    CHECK(!strcmp(eval_str(dv, "log.join(' ')+' '+d.open", &out), "cancel true"), "cancelled Escape: %s",
          au_buf_cstr(&out));
    eval_str(dv, "log=[]; window.keep=false", &out);
    key(dv, AU_KEY_ESCAPE, 0, 0);
    CHECK(!strcmp(eval_str(dv, "log.join(' ')+' '+d.open", &out), "cancel close false"), "Escape: %s",
          au_buf_cstr(&out));
    CHECK(!strcmp(focus_id(dv), "out2"), "close() returns the focus: %s", focus_id(dv));
    eval_str(dv, "log=[]", &out);
    click(dv, "out");
    CHECK(!strcmp(eval_str(dv, "log.join(' ')", &out), "down:out click:out"), "click after close: %s",
          au_buf_cstr(&out));
    au_docview_free(dv);

    /* the inert attribute: not focusable by Tab or click, not found */
    dv = view(&c, "<input id=a><div inert><input id=b>hidden words</div><input id=c>", false);
    key(dv, AU_KEY_TAB, 0, 0);
    key(dv, AU_KEY_TAB, 0, 0);
    CHECK(!strcmp(focus_id(dv), "c"), "Tab skips inert: %s", focus_id(dv));
    click(dv, "b");
    CHECK(strcmp(focus_id(dv), "b") != 0, "a click on an inert input focused it");
    CHECK(!au_layout_find(au_docview_layout(dv), "hidden", NULL, true, true, &m), "find matched inert text");
    au_docview_free(dv);

    /* popovers: Escape hides an auto popover, a click outside too, and a
     * click inside it or on its button keeps it */
    dv = view(&c,
              "<style>body{margin:0} [popover]{border:0;padding:0;width:100px;height:50px}</style>"
              "<button id=b popovertarget=p>toggle</button><div id=far style='height:400px'></div>"
              "<div id=p popover><span id=ps>in</span></div>",
              true);
    click(dv, "b");
    CHECK(!strcmp(eval_str(dv, "String(document.getElementById('p').matches(':popover-open'))", &out), "true"),
          "popovertarget click shows: %s", au_buf_cstr(&out));
    click(dv, "ps");
    CHECK(!strcmp(eval_str(dv, "String(document.getElementById('p').matches(':popover-open'))", &out), "true"),
          "a click inside hid it");
    key(dv, AU_KEY_ESCAPE, 0, 0);
    CHECK(!strcmp(eval_str(dv, "String(document.getElementById('p').matches(':popover-open'))", &out), "false"),
          "Escape left the popover open");
    click(dv, "b");
    click(dv, "far");
    CHECK(!strcmp(eval_str(dv, "String(document.getElementById('p').matches(':popover-open'))", &out), "false"),
          "light dismiss left the popover open");
    click(dv, "b");
    click(dv, "b");
    CHECK(!strcmp(eval_str(dv, "String(document.getElementById('p').matches(':popover-open'))", &out), "false"),
          "a second click on its button left the popover open");
    au_docview_free(dv);
    au_buf_free(&out);
}

static void test_edit_commands(void)
{
    Ctx c;
    AuDocView *dv = view(&c, "<input id=q value=\"hello world\"><input id=r>", false);
    click(dv, "q");
    au_docview_edit(dv, AU_EDITCMD_SELECT_ALL, NULL, 0, NULL);
    AuBuf out;
    au_buf_init(&out);
    CHECK(au_docview_can_edit(dv, AU_EDITCMD_CUT), "cannot cut a selection");
    au_docview_edit(dv, AU_EDITCMD_CUT, NULL, 0, &out);
    size_t n;
    CHECK(!strcmp(au_buf_cstr(&out), "hello world") && !strcmp(au_ctl_value(by_id(dv, "q"), &n), ""), "cut: %s",
          au_buf_cstr(&out));
    au_docview_edit(dv, AU_EDITCMD_UNDO, NULL, 0, NULL);
    CHECK(!strcmp(au_ctl_value(by_id(dv, "q"), &n), "hello world"), "undo cut");
    click(dv, "r");
    au_docview_edit(dv, AU_EDITCMD_PASTE, "a\nb", 3, NULL);
    CHECK(!strcmp(au_ctl_value(by_id(dv, "r"), &n), "a b"), "paste in one line: %s", au_ctl_value(by_id(dv, "r"), &n));
    au_buf_free(&out);
    au_docview_free(dv);
}

/* Painting a textarea wraps its text. The wrap measured the rest of the
 * text for every line and every prefix up to each space, so 80,000
 * characters took 11 s to paint (found by fuzz_layout). The cost must
 * grow with the text, not with its square: four times the text may take
 * at most ten times as long. */
static double textarea_paint_seconds(size_t chars, bool spaces)
{
    AuBuf b;
    au_buf_init(&b);
    au_buf_puts(&b, "<!DOCTYPE html><textarea rows=4 cols=40>");
    for (size_t i = 0; i < chars; i++)
        au_buf_putc(&b, spaces && i % 7 == 6 ? ' ' : 'x');
    au_buf_puts(&b, "</textarea>");
    au_buf_putc(&b, 0);
    Ctx c;
    AuDocView *dv = view(&c, b.data, false);
    AuSurface *s = au_surface_new(400, 200);
    clock_t t0 = clock();
    for (int k = 0; k < 3; k++) {
        AuCanvas *cv = au_canvas_new(s);
        au_docview_paint(dv, cv, (AuRect){0, 0, 400, 200}, 0, 0);
        au_canvas_free(cv);
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    au_surface_free(s);
    au_docview_free(dv);
    au_buf_free(&b);
    return secs;
}

static void test_textarea_wrap_scaling(void)
{
    for (int spaces = 0; spaces < 2; spaces++) {
        double small = textarea_paint_seconds(10000, spaces != 0);
        double large = textarea_paint_seconds(40000, spaces != 0);
        CHECK(large < 10 * small + 0.05, "textarea wrap, spaces %d: 10000 characters %.3f s, 40000 characters %.3f s",
              spaces, small, large);
    }
}

/* Script that changes a top-level document leaves the repaint to
 * au_docview_settle, which invalidates only what changed: nothing for a
 * restyle that computes the same values, the box for a colour change, and
 * the page when boxes moved. Reddit invalidated its whole page after each
 * script turn before this. */
static void settle_step(Ctx *c, AuDocView *dv, const char *js)
{
    AuBuf out;
    c->invals = 0;
    au_buf_init(&out);
    au_docview_eval(dv, js, &out);
    for (int i = 0; i < 5; i++)
        au_docview_run_scripts(dv, 10);
    CHECK(au_docview_settle_pending(dv) && !c->invals, "after %s: settle pending %d, %d invalidations (%s)", js,
          au_docview_settle_pending(dv), c->invals, au_buf_cstr(&out));
    au_buf_free(&out);
    au_docview_settle(dv);
}

static void test_settle_damage(void)
{
    Ctx c;
    AuDocView *dv = view(&c,
                         "<!DOCTYPE html><body style='margin:0'>"
                         "<div id=a style='height:20px'>A</div>"
                         "<div id=b style='margin-top:300px;height:20px;width:100px'>B</div>"
                         "<script>var a=document.getElementById('a'),b=document.getElementById('b'),queue=[];window.addEventListener('message',function(){queue.shift()()});"
                         "function later(f){queue.push(f);postMessage('go','*')}</script>",
                         true);
    AuRect rb;
    au_layout_element_rect(au_docview_layout(dv), by_id(dv, "b"), &rb);
    au_docview_settle(dv); /* what the load left */
    settle_step(&c, dv, "later(function(){a.className='nothing-matches'});''");
    CHECK(c.invals == 0, "same-value restyle: %d invalidations", c.invals);
    settle_step(&c, dv, "later(function(){b.style.color='red'});''");
    CHECK(c.invals >= 1 && c.inval.y0 >= (int)rb.y - 6 && c.inval.y1 <= (int)(rb.y + rb.h) + 6,
          "colour change: %d invalidations, %d,%d-%d,%d (box at y %g)", c.invals, c.inval.x0, c.inval.y0,
          c.inval.x1, c.inval.y1, rb.y);
    settle_step(&c, dv, "later(function(){a.style.height='40px'});''");
    CHECK(c.invals >= 1 && c.inval.y0 <= 0 && c.inval.y1 >= 600, "relayout: %d invalidations, %d,%d-%d,%d",
          c.invals, c.inval.x0, c.inval.y0, c.inval.x1, c.inval.y1);
    CHECK(!au_docview_settle_pending(dv), "settled");
    au_docview_free(dv);
}

/* Script reads and sets the scroll offset of a box that clips, and
 * getBoundingClientRect follows it and transforms, as in Chrome (the
 * same figures in tools/chrome-shot.py). MDN scrolls its sidebar to the
 * current page; the offset must also survive a rebuild of the box tree. */
static void test_element_scroll(void)
{
    Ctx c;
    AuDocView *dv = view(&c,
                         "<!DOCTYPE html><body style='margin:0'>"
                         "<div id=s style='overflow:auto;height:100px;width:200px;border:2px solid'>"
                         "<div style='height:50px'>a</div><div style='height:50px'>b</div><div style='height:50px'>c</div>"
                         "<div id=t style='height:50px'>target</div><div style='height:200px'>tail</div></div>"
                         "<div id=x style='transform:translateX(40px);width:50px;height:20px'></div>"
                         "<div id=h style='overflow:hidden;width:100px;height:30px'><div style='width:400px;height:30px'></div></div>",
                         true);
    static const struct {
        const char *js, *want;
    } steps[] = {
        {"s=document.getElementById('s');t=document.getElementById('t');s.scrollHeight+' '+s.clientHeight+' '+s.scrollTop",
         "400 100 0"},
        {"s.scrollTop=60;s.scrollTop+' '+Math.round(t.getBoundingClientRect().top)", "60 92"},
        {"t.scrollIntoView({block:'center'});s.scrollTop+' '+Math.round(t.getBoundingClientRect().top)", "125 27"},
        {"s.scrollTo({top:9999});s.scrollTop", "300"},
        {"s.scrollBy(0,-50);s.scrollTop", "250"},
        {"document.getElementById('x').getBoundingClientRect().left", "40"},
        {"h=document.getElementById('h');h.scrollLeft=120;h.scrollLeft+' '+h.scrollWidth", "120 400"},
        {"document.head.appendChild(document.createElement('style')).textContent='p{color:red}';"
         "document.body.offsetWidth;s.scrollTop",
         "250"},
    };
    AuBuf out;
    au_buf_init(&out);
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        au_buf_clear(&out);
        au_docview_eval(dv, steps[i].js, &out);
        CHECK(!strcmp(au_buf_cstr(&out), steps[i].want), "%s: %s, want %s", steps[i].js, au_buf_cstr(&out),
              steps[i].want);
    }
    au_buf_free(&out);
    au_docview_free(dv);
}

/* A CSS animation that repeats forever plays on the document's clock:
 * the box's drawn rectangle follows the keyframes, each frame repaints
 * only where the box was and is, and a finite animation does not play. */
static void test_css_animation(void)
{
    Ctx c;
    AuDocView *dv = view(&c,
                         "<!DOCTYPE html><style>@keyframes grow{from{transform:scale(1)}to{transform:scale(3)}}"
                         "@keyframes fade{50%{opacity:0}}</style><body style='margin:0'>"
                         "<div id=g style='margin:100px;width:10px;height:10px;animation:grow 1s linear infinite'></div>"
                         "<div id=o style='width:10px;height:10px;animation:fade 1s 2'></div>",
                         true);
    AuLayout *L = au_docview_layout(dv);
    CHECK(au_docview_animating(dv), "an infinite animation animates");
    uint32_t next = au_docview_animate(dv, 1000); /* the first frame: time 0 */
    CHECK(next <= 100, "next frame in %u ms", next);
    c.invals = 0;
    au_docview_animate(dv, 1500); /* half way: scale 2 */
    AuRect r;
    CHECK(au_layout_element_visual_rect(L, by_id(dv, "g"), 0, 0, &r) && fabsf(r.w - 20) < 0.5f &&
              fabsf(r.x - 95) < 0.5f,
          "scaled about its centre: %g,%g %gx%g", r.x, r.y, r.w, r.h);
    CHECK(c.invals >= 1 && c.inval.x0 >= 85 && c.inval.x1 <= 125 && c.inval.y1 <= 125,
          "repaints only the box: %d invalidations, %d,%d-%d,%d", c.invals, c.inval.x0, c.inval.y0, c.inval.x1,
          c.inval.y1);
    AuBuf out;
    au_buf_init(&out);
    au_docview_eval(dv, "document.getElementById('g').style.animation='none';''", &out);
    au_buf_free(&out);
    au_docview_update(dv);
    au_docview_animate(dv, 1600);
    CHECK(!au_docview_animating(dv), "animation none stops it");
    au_docview_free(dv);
}

int main(void)
{
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    test_editing();
    test_encodings();
    test_focus_order();
    test_typing_and_get();
    test_post_and_multipart();
    test_radio_and_select();
    test_validation();
    test_script_events();
    test_modal_dialog();
    test_pointer_events();
    test_inner_scroll_event();
    test_flex_scroller_keeps_offset();
    test_contenteditable();
    test_search_box_defer();
    test_keystroke_cost();
    test_edit_commands();
    test_textarea_wrap_scaling();
    test_settle_damage();
    test_element_scroll();
    test_css_animation();
    au_fontset_free(fonts);
    printf("forms: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
