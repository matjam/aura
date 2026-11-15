/* Form control tests: the editing model (src/page/editing.c), entry
 * lists and their encodings (formdata.c, against the HTML Standard's
 * algorithms), and through the document view: focus order, typing,
 * radio groups, selects, GET and POST submission, validation, and the
 * order of script events. */
#include "../layout/ahem.h"
#include "page/editing.h"
#include "page/formdata.h"
#include "page/page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
} Ctx;

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
    test_search_box_defer();
    test_edit_commands();
    au_fontset_free(fonts);
    printf("forms: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
