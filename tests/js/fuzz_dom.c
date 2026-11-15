/* Deterministic fuzzer for the DOM bindings with script on: wrapper
 * lifetimes, mutation hooks, ranges, and layout on a tree that script
 * keeps changing. Each iteration generates a script: random sequences of
 * DOM calls on a pool of nodes (create, append, insert, remove, replace,
 * clone, adopt, setAttribute, class and style changes, innerHTML,
 * outerHTML, insertAdjacentHTML, text edits, Range and Selection
 * operations, fragments, templates, shadow roots, custom elements, form
 * controls, style sheet edits, event listeners, observers and timers
 * whose callbacks mutate the tree again), each call in its own try
 * block. The script is split into steps; after each step the fuzzer
 * forces a layout, paints, hit tests, sends mouse and key events and
 * runs timers, so layout and input see every intermediate tree.
 *
 * The document view is freed at the end with script objects alive. An
 * iteration over AURA_FUZZ_SLOW_MS (default 60000) of CPU time aborts;
 * with AURA_FUZZ_VERBOSE set, scripts that take over 3 s are saved.
 * Run it under AddressSanitizer: the bugs it looks for are uses of freed
 * nodes and wrappers.
 *
 * usage: fuzz_dom ITERATIONS SEED SECONDS
 *        fuzz_dom replay SCRIPT.js... */
#include "../fuzz.h"
#include "../layout/ahem.h"
#include "page/page.h"

#include <stdarg.h>

#define STEP_MARK "\n//@@step\n"

static const char page[] =
    "<!DOCTYPE html><html><head><title>t</title><style id=st>.c{float:left}.d{display:none}p{margin:4px}"
    "div:has(> span){color:red}li:nth-child(odd){display:inline-block}</style></head>"
    "<body><div id=a class=c><p id=p1>one <b id=b1>two</b> three</p><span id=s1>span <i>i</i></span></div>"
    "<ul id=u1><li>a</li><li id=l2 class=d>b</li><li>c</li></ul>"
    "<table id=t1><tbody><tr><td id=d1>1</td><td>2</td></tr><tr><td colspan=2>3</td></tr></tbody></table>"
    "<form id=f1><input id=i1 value=v><input type=checkbox id=i2><select id=se><option>x</option><option selected>y</option>"
    "</select><textarea id=ta>text</textarea><button id=bu>go</button></form>"
    "<template id=tp><p>tpl <b>b</b></p></template><svg id=sv width=20 height=20><rect id=r1 width=5 height=5></rect></svg>"
    "<div id=host></div><p id=last>last paragraph of text</p>"
    "<script>var E=[],R=[],T=[],L=[],O=[],n=0;"
    "function e(i){return E.length?E[i%E.length]:document.body}"
    "function r(i){return R.length?R[i%R.length]:document.createRange()}"
    "function t(i){return T.length?T[i%T.length]:document.createTextNode('t')}"
    "function add(x){if(x&&E.length<400)E.push(x);return x}"
    "function all(){var q=document.querySelectorAll('*');for(var i=0;i<q.length&&E.length<400;i++)E.push(q[i])}"
    "function texts(){var w=document.createTreeWalker(document,4),x;while((x=w.nextNode())&&T.length<200)T.push(x)}"
    "all();texts();</script></body></html>";

static const char *const tags[] = {"div", "span", "p", "b", "a", "ul", "li", "table", "tbody", "tr", "td", "input", "select",
                                   "option", "textarea", "button", "form", "img", "br", "template", "style", "script", "svg",
                                   "details", "summary", "label", "fieldset", "x-el", "iframe", "slot", "h1", "pre", "caption",
                                   "colgroup", "col", "optgroup", "canvas", "dialog", "html", "body", "head"};
static const char *const attrs[] = {"id", "class", "style", "title", "href", "src", "value", "type", "name", "hidden", "colspan",
                                    "rowspan", "width", "height", "dir", "open", "selected", "checked", "disabled", "for",
                                    "tabindex", "slot", "is", "contenteditable", "start", "size", "multiple", "rows", "cols",
                                    "data-x", "aria-label", "onclick", "media", "rel", "align", "bgcolor"};
static const char *const values[] = {"", "a", "c", "d", "c d", "p1", "x y z", "1", "0", "-1", "99999", "text", "checkbox",
                                     "radio", "hidden", "display:none", "display:block;float:left", "position:absolute;top:1e9px",
                                     "display:table-cell", "display:contents", "width:1e38px", "#a", "x.css", "rtl", "true",
                                     "n=n+1", "E.length=0", "stylesheet", "all", "display:flex;order:-1", "display:list-item",
                                     "overflow:scroll;height:10px", "transform:scale(2)", "content:\"x\""};
static const char *const html[] = {
    "", "x", "<b>b</b>", "<p>p<span>s</span></p>", "<table><tr><td>c", "<li>li", "<tr><td>x</td></tr>", "<option>o",
    "<input value=q><textarea>t</textarea>", "<div class=c><div class=d>in</div></div>", "<style>p{display:none}</style>",
    "<svg><circle r=3 /></svg>", "<x-el></x-el>", "<template><i>t</i></template>", "<script>n++</script>",
    "<img src=x onerror=n++>", "<select><option>1<option>2</select>", "text only &amp; entity", "<details open><summary>s</summary>d</details>",
    "<a href=#last>link</a>", "<p id=p1>dup id</p>", "<iframe></iframe>", "<form><button>b</button></form>",
    "<div style=display:contents><span>c</span></div>", "<ol><li><ol><li>deep</ol></ol>",
};
static const char *const props[] = {"display", "position", "float", "width", "height", "margin", "padding", "border", "font-size",
                                    "overflow", "visibility", "content", "transform", "opacity", "z-index", "flex", "top",
                                    "white-space", "line-height", "color", "background", "--v"};
static const char *const pvals[] = {"none", "block", "inline", "inline-block", "flex", "table", "table-row", "table-cell",
                                    "list-item", "contents", "absolute", "fixed", "relative", "left", "right", "0", "10px",
                                    "1e9px", "-1e9px", "50%", "auto", "hidden", "scroll", "pre", "red", "scale(0)", "var(--v)",
                                    "calc(1px/0)", "", "inherit"};
static const char *const events[] = {"click", "x", "input", "change", "focus", "blur", "DOMContentLoaded", "load", "slotchange",
                                     "mousedown", "keydown", "submit", "reset", "toggle", "error"};

static void put(FzBuf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    fz_puts(b, tmp);
}

#define PICK(a) a[fz_rnd(sizeof a / sizeof a[0])]
#define N() fz_rnd(1000)

/* One statement that mutates the tree; used at the top level and inside
 * callbacks. depth limits callbacks inside callbacks. */
static void gen_op(FzBuf *b, int depth);

static void gen_callback(FzBuf *b, int depth)
{
    int k, n = 1 + (int)fz_rnd(3);
    fz_puts(b, "function(ev){");
    for (k = 0; k < n; k++)
        gen_op(b, depth + 1);
    fz_puts(b, "}");
}

static void gen_op(FzBuf *b, int depth)
{
    uint32_t op = fz_rnd(depth > 1 ? 40 : 72);
    fz_puts(b, "try{");
    switch (op) {
    case 0: put(b, "add(document.createElement('%s'))", PICK(tags)); break;
    case 1: put(b, "e(%u).appendChild(e(%u))", N(), N()); break;
    case 2: put(b, "e(%u).insertBefore(e(%u),e(%u).firstChild)", N(), N(), N()); break;
    case 3: put(b, "e(%u).remove()", N()); break;
    case 4: put(b, "e(%u).removeChild(e(%u).lastChild)", N(), N()); break;
    case 5: put(b, "e(%u).replaceWith(e(%u),'txt',e(%u))", N(), N(), N()); break;
    case 6: put(b, "e(%u).replaceChild(e(%u),e(%u).firstChild)", N(), N(), N()); break;
    case 7: put(b, "add(e(%u).cloneNode(%s))", N(), fz_rnd(2) ? "true" : "false"); break;
    case 8: put(b, "e(%u).setAttribute('%s','%s')", N(), PICK(attrs), PICK(values)); break;
    case 9: put(b, "e(%u).removeAttribute('%s')", N(), PICK(attrs)); break;
    case 10: put(b, "e(%u).className='%s'", N(), PICK(values)); break;
    case 11: put(b, "e(%u).classList.toggle('%s')", N(), fz_rnd(2) ? "c" : "d"); break;
    case 12: put(b, "e(%u).style.setProperty('%s','%s')", N(), PICK(props), PICK(pvals)); break;
    case 13: put(b, "e(%u).style.cssText='%s:%s;%s:%s'", N(), PICK(props), PICK(pvals), PICK(props), PICK(pvals)); break;
    case 14: put(b, "e(%u).innerHTML='%s'", N(), PICK(html)); break;
    case 15: put(b, "e(%u).outerHTML='%s'", N(), PICK(html)); break;
    case 16: {
        static const char *const where[] = {"beforebegin", "afterbegin", "beforeend", "afterend"};
        put(b, "e(%u).insertAdjacentHTML('%s','%s')", N(), PICK(where), PICK(html));
        break;
    }
    case 17: put(b, "e(%u).textContent='%s'", N(), fz_rnd(3) ? "new text here" : ""); break;
    case 18: put(b, "T.push(document.createTextNode('some text %u'));e(%u).appendChild(t(%u))", N(), N(), N()); break;
    case 19: put(b, "T.push(t(%u).splitText(%u))", N(), fz_rnd(12)); break;
    case 20: put(b, "t(%u).data='%s'", N(), fz_rnd(2) ? "d" : "longer data \\ud83d\\ude00 x"); break;
    case 21: put(b, "t(%u).deleteData(%u,%u)", N(), fz_rnd(8), fz_rnd(8)); break;
    case 22: put(b, "t(%u).remove()", N()); break;
    case 23: put(b, "e(%u).normalize()", N()); break;
    case 24: put(b, "e(%u).offsetWidth;e(%u).getBoundingClientRect()", N(), N()); break;
    case 25: put(b, "getComputedStyle(e(%u)).%s", N(), fz_rnd(2) ? "width" : "display"); break;
    case 26: put(b, "e(%u).append(e(%u),'s',t(%u))", N(), N(), N()); break;
    case 27: put(b, "e(%u).prepend(e(%u))", N(), N()); break;
    case 28: put(b, "e(%u).before(e(%u));e(%u).after(t(%u))", N(), N(), N(), N()); break;
    case 29: put(b, "e(%u).replaceChildren(e(%u),e(%u))", N(), N(), N()); break;
    case 30: put(b, "e(%u).id='%s'", N(), fz_rnd(2) ? "p1" : "zz"); break;
    case 31: put(b, "e(%u).hidden=%s", N(), fz_rnd(2) ? "true" : "false"); break;
    case 32: put(b, "e(%u).focus()", N()); break;
    case 33: put(b, "e(%u).scrollIntoView();e(%u).scrollTop=%u", N(), N(), fz_rnd(100)); break;
    case 34: put(b, "e(%u).value='%s';e(%u).checked=true;e(%u).selectedIndex=%u", N(), PICK(values), N(), N(), fz_rnd(3)); break;
    case 35: put(b, "document.body.appendChild(e(%u))", N()); break;
    case 36: put(b, "e(%u).parentNode.removeChild(e(%u))", N(), N()); break;
    case 37: put(b, "all()"); break;
    case 38: put(b, "texts()"); break;
    case 39: put(b, "e(%u).toggleAttribute('%s')", N(), PICK(attrs)); break;
    /* ---- top level and first-level callbacks only ---- */
    case 40: put(b, "R.push(document.createRange())"); break;
    case 41: put(b, "r(%u).setStart(%s(%u),%u);r(%u).setEnd(%s(%u),%u)", N(), fz_rnd(2) ? "e" : "t", N(), fz_rnd(6), N(),
                 fz_rnd(2) ? "e" : "t", N(), fz_rnd(9));
        break;
    case 42: put(b, "r(%u).selectNode(e(%u))", N(), N()); break;
    case 43: put(b, "r(%u).selectNodeContents(e(%u))", N(), N()); break;
    case 44: put(b, "r(%u).deleteContents()", N()); break;
    case 45: put(b, "add(r(%u).extractContents())", N()); break;
    case 46: put(b, "add(r(%u).cloneContents())", N()); break;
    case 47: put(b, "r(%u).insertNode(e(%u))", N(), N()); break;
    case 48: put(b, "r(%u).surroundContents(e(%u))", N(), N()); break;
    case 49: put(b, "r(%u).collapse(%s);r(%u).getBoundingClientRect();r(%u).toString()", N(), fz_rnd(2) ? "true" : "false", N(), N());
        break;
    case 50: put(b, "var s=getSelection();s.removeAllRanges();s.addRange(r(%u));s.deleteFromDocument()", N()); break;
    case 51: put(b, "getSelection().setBaseAndExtent(t(%u),%u,e(%u),%u);getSelection().extend(e(%u),0)", N(), fz_rnd(5), N(),
                 fz_rnd(3), N());
        break;
    case 52: put(b, "var f=document.createDocumentFragment();f.append(e(%u),e(%u));add(f);e(%u).appendChild(f)", N(), N(), N()); break;
    case 53: put(b, "add(e(%u).content);e(%u).content.appendChild(e(%u))", N(), N(), N()); break;
    case 54: put(b, "var sr=e(%u).attachShadow({mode:'open'});add(sr);sr.innerHTML='<slot></slot><p>sh</p>'", N()); break;
    case 55: put(b, "add(e(%u).shadowRoot).appendChild(e(%u))", N(), N()); break;
    case 56:
        put(b, "customElements.define('x-el',class extends HTMLElement{connectedCallback(){var ev;");
        gen_op(b, 2);
        put(b, "}disconnectedCallback(){var ev;");
        gen_op(b, 2);
        put(b, "}static get observedAttributes(){return['class','id']}attributeChangedCallback(){var ev;");
        gen_op(b, 2);
        put(b, "}})");
        break;
    case 57:
        put(b, "var h=");
        gen_callback(b, depth);
        put(b, ";L.push(h);e(%u).addEventListener('%s',h,%s)", N(), PICK(events), fz_rnd(4) ? "false" : "{once:true,capture:true}");
        break;
    case 58: put(b, "e(%u).dispatchEvent(new Event('%s',{bubbles:true,cancelable:true}))", N(), PICK(events)); break;
    case 59: put(b, "e(%u).click()", N()); break;
    case 60:
        /* At most three: every observer gets a record for every change,
         * and their callbacks change the tree again. */
        put(b, "if(O.length<3){var mo=new MutationObserver(");
        gen_callback(b, depth);
        put(b, ");O.push(mo);mo.observe(e(%u),{childList:true,subtree:true,attributes:true,characterData:true})}", N());
        break;
    case 61:
        put(b, "setTimeout(");
        gen_callback(b, depth);
        put(b, ",%u)", fz_rnd(3));
        break;
    case 62:
        put(b, "requestAnimationFrame(");
        gen_callback(b, depth);
        put(b, ")");
        break;
    case 63: put(b, "document.adoptNode(e(%u));add(document.importNode(e(%u),true))", N(), N()); break;
    case 64: put(b, "var d2=document.implementation.createHTMLDocument('x');d2.body.appendChild(e(%u));add(d2.body);add(d2)", N()); break;
    case 65: put(b, "var sh=document.getElementById('st').sheet;sh.insertRule('%s{%s:%s}',0);if(sh.cssRules.length>6)sh.deleteRule(1)",
                 PICK(tags), PICK(props), PICK(pvals));
        break;
    case 66: put(b, "document.getElementById('st').textContent='%s{%s:%s}.c{%s:%s}'", PICK(tags), PICK(props), PICK(pvals), PICK(props),
                 PICK(pvals));
        break;
    case 67: put(b, "E.splice(%u%%(E.length||1),%u);R.splice(0,%u);T.splice(%u%%(T.length||1),%u)", N(), 1 + fz_rnd(30), fz_rnd(3), N(),
                 1 + fz_rnd(20));
        break;
    case 68: put(b, "document.body.innerHTML='%s%s'", PICK(html), PICK(html)); break;
    case 69: put(b, "var w=document.createTreeWalker(e(%u),-1),x,k=0;while((x=w.nextNode())&&k++<20){if(k%%3==0)x.remove();else add(x)}", N());
        break;
    case 70: put(b, "e(%u).form&&e(%u).form.reset();e(%u).options.length=%u;e(%u).add(new Option('n'))", N(), N(), N(), fz_rnd(3), N()); break;
    default: put(b, "document.title='t%u';document.documentElement.appendChild(e(%u))", N(), N()); break;
    }
    fz_puts(b, "}catch(_){}\n");
}

static void generate(FzBuf *b)
{
    int steps = 2 + (int)fz_rnd(8), s, k;
    b->n = 0;
    for (s = 0; s < steps; s++) {
        int n = 1 + (int)fz_rnd(14);
        if (s)
            fz_puts(b, STEP_MARK);
        for (k = 0; k < n; k++)
            gen_op(b, 0);
    }
}

static AuFontSet *fonts;
static AuSurface *surface;
static long slow_ms = 60000;

static double fake_now;
static double now_ms(void *ctx, AuDocView *dv)
{
    (void)ctx, (void)dv;
    fake_now += 1;
    return fake_now;
}

static void between_steps(AuDocView *dv)
{
    AuCanvas *c;
    AuHitResult hr;
    AuEvent ev;
    int cw, ch, m;
    au_docview_update(dv);
    au_docview_content_size(dv, &cw, &ch);
    c = au_canvas_new(surface);
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 150}, 0, 0);
    au_canvas_free(c);
    for (m = 0; m < 3; m++) {
        memset(&ev, 0, sizeof ev);
        ev.type = m == 0 ? AU_EVENT_MOUSE_MOVE : m == 1 ? AU_EVENT_MOUSE_DOWN : AU_EVENT_MOUSE_UP;
        if (m != 2) {
            ev.x = (int)fz_rnd(300);
            ev.y = (int)fz_rnd(200);
        }
        ev.clicks = 1;
        au_docview_mouse(dv, &ev);
        au_docview_hit_test(dv, ev.x, ev.y, &hr);
    }
    memset(&ev, 0, sizeof ev);
    ev.type = AU_EVENT_KEY_DOWN;
    if (fz_rnd(2))
        ev.key = fz_rnd(2) ? AU_KEY_TAB : AU_KEY_RETURN;
    else
        ev.ch = 'k';
    au_docview_key(dv, &ev);
    for (m = 0; m < 4; m++)
        au_docview_run_scripts(dv, 5);
    au_docview_update(dv);
}

static void run_one(const FzBuf *in)
{
    AuDocViewCallbacks cb;
    AuDocView *dv;
    const char *p = (const char *)in->d, *end = p + in->n;
    size_t mark = strlen(STEP_MARK);
    int steps = 0;
    memset(&cb, 0, sizeof cb);
    cb.now_ms = now_ms;
    fake_now = 1000;
    dv = au_docview_new(fonts, NULL, &cb);
    au_docview_set_scripting(dv, true);
    au_docview_set_viewport(dv, 300, 200);
    au_docview_load(dv, page, sizeof page - 1, "http://fuzz.example/page.html");
    au_docview_run_scripts(dv, 5);
    between_steps(dv);
    while (p < end && steps++ < 64) {
        const char *q = p;
        AuBuf out, src;
        while (q + mark <= end && memcmp(q, STEP_MARK, mark) != 0)
            q++;
        if (q + mark > end)
            q = end;
        au_buf_init(&out);
        au_buf_init(&src);
        au_buf_append(&src, p, (size_t)(q - p));
        au_buf_putc(&src, 0);
        if (!au_docview_eval(dv, src.data, &out) && getenv("AURA_FUZZ_VERBOSE"))
            printf("step %d: %.*s\n", steps, (int)out.len, out.data ? out.data : "");
        au_buf_free(&src);
        au_buf_free(&out);
        between_steps(dv);
        p = q < end ? q + mark : end;
    }
    if (fz_rnd(4) == 0) {
        /* a second document in the same view: the first one's wrappers
         * and listeners go with it */
        au_docview_load(dv, page, sizeof page - 1, "http://fuzz.example/page2.html");
        au_docview_run_scripts(dv, 5);
        between_steps(dv);
    }
    au_docview_free(dv);
}

static int fuzz_main(int argc, char **argv)
{
    long iters = 0, it;
    int first, i;
    FzBuf in = {0};
    static uint8_t fontdata[16384];
    int replay = fz_args(argc, argv, &iters, &first);

    if (getenv("AURA_FUZZ_SLOW_MS"))
        slow_ms = atol(getenv("AURA_FUZZ_SLOW_MS"));
    fonts = ahem_fontset(fontdata);
    surface = au_surface_new(200, 150);
    if (replay) {
        for (i = first; i < argc; i++) {
            if (!fz_read_file(argv[i], &in, 0))
                continue;
            fz_replay_seed(argv[i]);
            run_one(&in);
            printf("%s: ok\n", argv[i]);
        }
        return 0;
    }
    for (it = fz_first; it < iters && !fz_expired(); it++) {
        fz_iteration(it);
        clock_t t0 = clock();
        long ms;
        generate(&in);
        fz_save(in.d, in.n);
        run_one(&in);
        ms = (long)((double)(clock() - t0) * 1000 / CLOCKS_PER_SEC);
        if (ms > 3000 && getenv("AURA_FUZZ_VERBOSE")) {
            char name[64];
            FILE *f;
            snprintf(name, sizeof name, "fuzz-dom-slow-%ld.js", it);
            f = fopen(name, "wb");
            if (f) {
                fwrite(in.d, 1, in.n, f);
                fclose(f);
            }
            printf("iteration %ld took %ld ms (saved as %s)\n", it, ms, name);
            fflush(stdout);
        }
        if (ms > slow_ms) {
            printf("fuzz_dom: a script of %lu bytes took %ld ms\n", (unsigned long)in.n, ms);
            abort();
        }
    }
    printf("fuzz_dom: %ld iterations\n", it - fz_first);
    au_surface_free(surface);
    au_fontset_free(fonts);
    return 0;
}

int main(int argc, char **argv) { return fz_main(fuzz_main, argc, argv); }
