/* Subtree rebuilds after DOM changes (src/layout/rebuild.c) and targeted
 * restyles of child insertions and removals (au_css_invalidate_children).
 *
 * Each case loads a page with script on, runs a script that changes the
 * DOM, and updates the document view with the differential check on: the
 * targeted restyle is compared with a whole restyle, and the rebuilt box
 * tree, laid out, with a whole build. A case says whether the change must
 * stay incremental (no whole build) or may fall back to one.
 *
 *   test_incremental [-v]
 */
#include "ahem.h"
#include "page/page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
static bool verbose;

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

enum { INCR, FALLBACK, ANY };

typedef struct Case {
    const char *name;
    const char *style;
    const char *body;
    const char *script; /* run after the first layout; steps separated by "@@" get an update each */
    int expect;
} Case;

#define LIST_CSS "ul.m { list-style: none; margin: 0; padding: 0 } .m li:first-child { font-weight: bold } "

static const Case cases[] = {
    /* ---- Inserting and removing ---- */
    {"append to a block", "", "<div id=a><p>one</p></div><p>after</p>",
     "var p=document.createElement('p');p.textContent='two';a.appendChild(p)", INCR},
    {"remove from a block", "", "<div id=a><p>one</p><p id=x>two</p></div><p>after</p>", "x.remove()", INCR},
    {"insert before", "", "<div id=a><p id=x>one</p><p>two</p></div>",
     "var p=document.createElement('p');p.textContent='zero';a.insertBefore(p,x)", INCR},
    {"replace children", "", "<div id=a><p>one</p><p>two</p></div><div>after</div>",
     "a.innerHTML='<p>three</p><p>four</p><p>five</p>'", INCR},
    {"text data", "", "<div id=a><p>one <b id=b>two</b> three</p></div>", "b.firstChild.data='twenty two'", INCR},
    {"text node added", "", "<div id=a><p id=p>one</p></div>",
     "p.appendChild(document.createTextNode(' more text'))", INCR},
    {"several changes in one turn", "", "<div id=a><p id=x>one</p></div><div id=b><p>b</p></div><div id=c></div>",
     "x.textContent='x';b.appendChild(document.createElement('hr'));c.innerHTML='<i>c</i>';x.remove()", INCR},
    {"a node moves to another root", "", "<div id=a><p id=x>moving</p></div><div id=b><p>b</p></div>",
     "b.appendChild(x)", INCR},
    {"remove then insert again", "", "<div id=a><p id=x>x</p><p>y</p></div>", "x.remove();@@a.appendChild(x)",
     INCR},
    {"comment only", "", "<div id=a><p>one</p></div>", "a.appendChild(document.createComment('c'))", INCR},
    {"many parents", "", "<div id=a></div>",
     "for(var i=0;i<100;i++){var d=document.createElement('div');d.id='d'+i;a.appendChild(d);}@@"
     "for(var i=0;i<100;i++){document.getElementById('d'+i).textContent='t'+i;}",
     ANY},
    /* ---- Anonymous blocks and inline splits ---- */
    {"block into inline content", "", "<div id=a>text <span>inline</span> more</div>",
     "var d=document.createElement('div');d.textContent='block';a.insertBefore(d,a.lastChild)", INCR},
    {"inline between blocks", "", "<div id=a><p>one</p><p id=x>two</p></div>",
     "a.insertBefore(document.createTextNode(' loose text '),x)", INCR},
    {"block inside an inline", "", "<div id=a>before <span id=s>in <b>bold</b> span</span> after</div>",
     "var d=document.createElement('div');d.textContent='split';s.insertBefore(d,s.lastChild)", INCR},
    {"remove the block from an inline", "", "<div id=a>x <span>in <div id=d>block</div> out</span> y</div>",
     "d.remove()", INCR},
    {"white space at the edges", "p span { color: red }", "<div id=a><p id=p>a <span id=s> b </span> c</p></div>",
     "s.firstChild.data='  ';@@s.appendChild(document.createTextNode(' d '))", INCR},
    {"white space after a float", ".f { float: left; width: 30px }",
     "<div id=a><p>a <span class=f id=f>float</span> b <i>c</i></p></div>", "f.textContent='floating'", INCR},
    {"preformatted", "pre { white-space: pre }", "<pre id=p>a  b\n  c</pre>",
     "p.appendChild(document.createTextNode('\\n  d  '))", INCR},
    /* ---- Generated content, lists and counters ---- */
    {"before and after", "#a::before { content: '['; } #a::after { content: ']' }", "<div id=a><p>x</p></div>",
     "a.appendChild(document.createElement('p')).textContent='y'", INCR},
    {"list items without numbers", LIST_CSS, "<div><ul class=m id=u><li>one</li></ul></div><ol><li>n</li></ol>",
     "u.appendChild(document.createElement('li')).textContent='two'", INCR},
    {"numbered list", "", "<div id=w><ol id=o><li>one</li><li>two</li></ol><ol><li>n</li></ol></div>",
     "var l=document.createElement('li');l.textContent='zero';o.insertBefore(l,o.firstChild)", INCR},
    {"reversed list", "", "<div><ol reversed id=o><li>one</li><li>two</li></ol></div>",
     "o.appendChild(document.createElement('li')).textContent='three'", INCR},
    {"list item value", "", "<div><ol id=o><li>a</li><li id=v>b</li><li>c</li></ol></div>",
     "v.setAttribute('value','10')", INCR},
    {"counter in content",
     "body { counter-reset: n } h2::before { counter-increment: n; content: counter(n) '. ' }",
     "<div id=a><h2>one</h2></div><h2>two</h2>", "a.appendChild(document.createElement('h2')).textContent='new'",
     ANY},
    {"local counter", ".c { counter-reset: k } .c p::before { counter-increment: k; content: counter(k) ': ' }",
     "<section><div class=c id=c><p>a</p></div></section><p>out</p>",
     "c.appendChild(document.createElement('p')).textContent='b'", INCR},
    {"quotes", "", "<div id=a><p><q>x</q></p></div>",
     "a.appendChild(document.createElement('p')).innerHTML='<q>y</q>'", FALLBACK},
    /* ---- Tables ---- */
    {"cell into a row", "", "<div><table id=t><tr id=r><td>a</td></tr></table></div>",
     "r.appendChild(document.createElement('td')).textContent='b'", INCR},
    {"row into a body", "", "<div><table><tbody id=b><tr><td>a</td><td>b</td></tr></tbody></table></div>",
     "b.appendChild(document.createElement('tr')).innerHTML='<td colspan=2>c</td>'", INCR},
    {"stray cell", "", "<div id=a><p>x</p></div>", "a.appendChild(document.createElement('td')).textContent='cell'",
     INCR},
    {"percentage margins on a table", "table { margin-right: 9%; margin-bottom: 29% }",
     "<div id=a><table><tr><td>a</td></tr></table></div>", "a.appendChild(document.createElement('table'))", INCR},
    {"text in a cell", "", "<table><tr><td id=c>a</td><td>b</td></tr></table>", "c.textContent='a longer text'",
     INCR},
    {"colspan", "", "<div><table><tr><td id=c>a</td><td>b</td></tr><tr><td>c</td><td>d</td></tr></table></div>",
     "c.setAttribute('colspan','2')", INCR},
    /* ---- Flex and grid ---- */
    {"flex item added", ".f { display: flex; gap: 4px }", "<div class=f id=f><div>a</div>text</div>",
     "f.appendChild(document.createElement('div')).textContent='b'", INCR},
    {"inside a flex item", ".f { display: flex }", "<div class=f><div id=i>a</div><div>b</div></div>",
     "i.appendChild(document.createElement('p')).textContent='c'", INCR},
    {"floated flex item", ".f { display: flex } .fl { float: left }",
     "<div class=f><div class=fl id=i>a</div><span>b</span></div>", "i.textContent='aa'", INCR},
    {"grid item", ".g { display: grid; grid-template-columns: 1fr 1fr }",
     "<div class=g id=g><div id=i>a</div><div>b</div></div>",
     "i.appendChild(document.createElement('b')).textContent='c';@@g.appendChild(document.createElement('div'))",
     INCR},
    {"inline-block", ".ib { display: inline-block }", "<p>a <span class=ib id=i>b</span> c</p>",
     "i.appendChild(document.createElement('div')).textContent='d'", INCR},
    {"inline flex", ".if { display: inline-flex }", "<p>a <span class=if id=i><b>b</b></span> c</p>",
     "i.appendChild(document.createElement('i')).textContent='d'", INCR},
    /* ---- Floats and positioned boxes ---- */
    {"inside a float", ".fl { float: right; width: 100px }", "<div><div class=fl id=f>a</div><p>text</p></div>",
     "f.appendChild(document.createElement('p')).textContent='b'", INCR},
    {"float added", ".fl { float: left; width: 50px }", "<div id=a><p>text text text</p></div>",
     "var d=document.createElement('div');d.className='fl';d.textContent='f';a.insertBefore(d,a.firstChild)", INCR},
    {"inside an absolute box", ".ab { position: absolute; top: 10px; left: 10px }",
     "<div style='position:relative'><div class=ab id=b>a</div><p>x</p></div>",
     "b.appendChild(document.createElement('div')).textContent='b'", INCR},
    {"fixed box in an inline", ".fx { position: fixed; top: 0 }", "<p>a <span><b class=fx id=f>f</b></span> c</p>",
     "f.textContent='fixed'", INCR},
    {"sticky header", "header { position: sticky; top: 0 }", "<header id=h><div id=s>s</div></header><p>p</p>",
     "s.appendChild(document.createElement('ul')).innerHTML='<li>x</li><li>y</li>'", INCR},
    {"relative box in a reused subtree", ".w { overflow: hidden } .r { position: relative; left: 10px; top: 3px }",
     "<div class=w><p class=r>r</p><span class=r>s</span></div><div id=a><p>x</p></div>",
     "a.appendChild(document.createElement('p')).textContent='y'", INCR},
    /* ---- display ---- */
    {"display contents", ".c { display: contents }", "<div><div class=c id=c><p>a</p></div></div>",
     "c.appendChild(document.createElement('p')).textContent='b'", INCR},
    {"display none", ".h { display: none }", "<div class=h id=h><p>a</p></div><p>b</p>",
     "h.appendChild(document.createElement('p')).textContent='c'", INCR},
    {"shown after insertion", ".h { display: none }", "<div class=h id=h><p>a</p></div><p>b</p>",
     "h.appendChild(document.createElement('p')).textContent='c';@@h.className=''", ANY},
    {"display changes with the insertion", "#a:has(.x) { display: flex }", "<section><div id=a><p>a</p></div></section>",
     "a.appendChild(document.createElement('p')).className='x'", ANY},
    /* ---- Style changes that change the boxes ---- */
    {"hidden and shown by class", ".x { display: none }", "<div><p id=p>a</p><p>b</p></div><p>c</p>",
     "p.className='x';@@p.className=''", INCR},
    {"floated by class", ".f { float: left; width: 20px }", "<div><p id=p>a</p><p>b b b b</p></div>",
     "p.className='f';@@p.className=''", INCR},
    {"positioned by class", ".a { position: absolute; top: 0 }", "<div><p>z</p><p id=p>a</p><p>b</p></div>",
     "p.className='a'", INCR},
    {"inline made a block", ".b { display: block }", "<div><p>x <span id=s>s</span> y</p></div>",
     "s.className='b';@@s.className=''", INCR},
    {"generated content changed", "#a::before { content: 'x' } #a.y::before { content: 'longer' }",
     "<div><p id=a>a</p></div>", "a.className='y';@@a.className=''", INCR},
    {"list style changed", "li.n { list-style-type: upper-roman }", "<div><ol><li id=l>a</li><li>b</li></ol></div>",
     "l.className='n'", INCR},
    {"flex container by class", ".f { display: flex }", "<section><div id=d>a <b>b</b> c</div></section>",
     "d.className='f';@@d.className=''", INCR},
    /* ---- Selectors that insertions reach ---- */
    {"first child", LIST_CSS, "<div><ul class=m id=u><li>one</li><li>two</li></ul></div>",
     "var l=document.createElement('li');l.textContent='zero';u.insertBefore(l,u.firstChild)", INCR},
    {"last child", "li:last-child { color: red } li:nth-last-child(2) { color: blue }",
     "<div><ul id=u><li>one</li><li>two</li><li id=x>three</li></ul></div>", "x.remove()", INCR},
    {"nth child", "p:nth-child(odd) { background: #eee } p:nth-of-type(3n) { margin-left: 5px }",
     "<div id=a><p>1</p><p>2</p><p>3</p><p>4</p></div>",
     "a.insertBefore(document.createElement('p'),a.children[1]).textContent='new'", INCR},
    {"only child", "p:only-child { color: green }", "<div id=a><p>1</p></div>",
     "a.appendChild(document.createElement('p')).textContent='2'", INCR},
    {"empty", "div:empty { height: 20px; background: red } div:empty + p { color: red }",
     "<div id=a></div><p>after</p>", "a.textContent='x';@@a.textContent=''", INCR},
    {"adjacent sibling", ".a + p { color: red } .a + p b { color: blue }",
     "<div id=w><p class=a>a</p><p id=x>b <b>c</b></p><p>d</p></div>",
     "w.insertBefore(document.createElement('p'),x).textContent='new';@@x.previousSibling.remove()", INCR},
    {"general sibling", ".a ~ p { color: red }", "<div id=w><p id=x>b</p><p>c</p></div>",
     "var p=document.createElement('p');p.className='a';w.insertBefore(p,x)", INCR},
    {"not last child", "li:not(:last-child) { border-bottom: 1px solid }", "<div><ul id=u><li>a</li></ul></div>",
     "u.appendChild(document.createElement('li')).textContent='b'", INCR},
    {"has child", ".card:has(> img) { padding: 10px } .card:has(.x) p { color: red }",
     "<div class=card id=c><p>t</p></div>",
     "c.appendChild(document.createElement('img'));@@c.appendChild(document.createElement('span')).className='x'", INCR},
    {"has sibling", "h2:has(+ .note) { color: red }", "<div id=w><h2>h</h2><p id=x>p</p></div>",
     "var n=document.createElement('p');n.className='note';w.insertBefore(n,x);@@n.remove()", INCR},
    {"has empty", ".w:has(p:empty) { color: red }", "<div class=w><p id=p>x</p></div>", "p.firstChild.data=''", INCR},
    /* ---- Form controls and other replaced content ---- */
    {"option added", "", "<div><select id=s><option>a</option></select></div>",
     "var o=document.createElement('option');o.textContent='a much longer option';o.selected=true;s.appendChild(o)",
     INCR},
    {"select value", "", "<div><select id=s><option>a</option><option>bbbbbb</option></select></div>",
     "s.value='bbbbbb'", INCR},
    {"options emptied", "", "<form id=f><select id=s><option>a</option><option selected>bbbbbb</option></select></form>",
     "f.reset();s.options.length=0;@@s.add(new Option('n'))", INCR},
    {"form reset", "", "<form id=f><select id=s><option>a</option><option>bbbbbb</option></select></form>",
     "s.selectedIndex=1;@@f.reset()", INCR},
    {"option text", "", "<div><select><option id=o>a</option></select></div>", "o.textContent='bbbbbbbb'", INCR},
    {"textarea text", "textarea:placeholder-shown { color: red }",
     "<div><textarea id=t placeholder=ph></textarea></div>", "t.textContent='typed'", INCR},
    {"default button", "button:default { color: red } input:indeterminate { color: blue }",
     "<form id=f><p id=p><button>a</button></p></form>",
     "f.insertBefore(document.createElement('button'),p).textContent='b'", ANY},
    {"picture source", "", "<div><picture id=p><img src='a.png' alt=a></picture></div>",
     "var s=document.createElement('source');s.setAttribute('srcset','b.png');p.insertBefore(s,p.firstChild)", INCR},
    {"details summary", "", "<div><details id=d><summary>s</summary><p>p</p></details></div>",
     "d.insertBefore(document.createElement('summary'),d.firstChild).textContent='first';@@d.open=true", ANY},
    /* (a summary is a list item: the list-item counter it increments is
     * created outside the rebuilt subtree, so the subtree build gives up) */
    {"fieldset legend", "", "<div><fieldset id=f><p>x</p></fieldset></div>",
     "f.insertBefore(document.createElement('legend'),f.firstChild).textContent='L'", INCR},
    {"inline svg child", "", "<div><svg id=s width=50 height=50><rect width=10 height=10 /></svg></div>",
     "var c=document.createElementNS('http://www.w3.org/2000/svg','circle');c.setAttribute('r','5');s.appendChild(c)",
     INCR},
    {"image src", "", "<div><p><img id=i alt=one> x</p></div>", "i.alt='a longer alternative text'", INCR},
    {"modal dialog", "", "<div><dialog id=d><p>x</p></dialog></div>",
     "d.showModal();@@d.appendChild(document.createElement('p')).textContent='y'", ANY},
    /* ---- Scroll containers ---- */
    {"scrolled list", ".s { height: 40px; overflow: auto }",
     "<div class=s id=s><p>1</p><p>2</p><p>3</p><p>4</p></div><p>after</p>",
     "s.scrollTop=20;@@s.appendChild(document.createElement('p')).textContent='5'", INCR},
    /* ---- The typeahead pattern ---- */
    {"suggestions",
     ".menu { position: absolute; list-style: none } .menu:empty { display: none } .menu li + li { border-top: 1px "
     "solid }",
     "<header><div class=search><input id=q><ul class=menu id=m></ul></div></header><article><ol><li>a</li></ol>"
     "<p>text</p></article>",
     "for(var k=0;k<3;k++){while(m.firstChild)m.removeChild(m.firstChild);for(var i=0;i<4;i++){var "
     "l=document.createElement('li');l.innerHTML='<span>s'+k+i+'</span>';m.appendChild(l);}}@@"
     "while(m.firstChild)m.removeChild(m.firstChild)",
     INCR},
};

static void run_case(const Case *c)
{
    AuDocView *dv = au_docview_new(fonts, NULL, NULL);
    au_docview_set_check(dv, true);
    au_docview_set_scripting(dv, true);
    au_docview_set_viewport(dv, 400, 300);
    AuBuf html;
    au_buf_init(&html);
    au_buf_printf(&html, "<!DOCTYPE html><html><head><style>body { font: 10px Ahem } %s</style></head><body>%s</body>"
                         "</html>",
                  c->style, c->body);
    au_docview_load(dv, html.data, html.len, "http://example.com/");
    for (int i = 0; i < 10; i++)
        au_docview_run_scripts(dv, 10);
    au_docview_update(dv);
    AuDocViewStats s0, s1;
    au_docview_stats(dv, &s0);
    /* Each step is evaluated, then the view updated. */
    const char *p = c->script;
    AuBuf step, out;
    au_buf_init(&step);
    au_buf_init(&out);
    while (*p) {
        const char *e = strstr(p, "@@");
        size_t n = e ? (size_t)(e - p) : strlen(p);
        au_buf_clear(&step);
        /* Elements with an id are named by it, as in a browser. */
        au_buf_puts(&step, "Array.prototype.forEach.call(document.querySelectorAll('[id]'),function(e){globalThis[e.id]=e});");
        au_buf_append(&step, p, n);
        au_buf_puts(&step, ";''");
        au_buf_clear(&out);
        bool ok = au_docview_eval(dv, au_buf_cstr(&step), &out);
        CHECK(ok, "%s: the script failed: %s", c->name, au_buf_cstr(&out));
        au_docview_update(dv);
        p += n;
        if (e)
            p += 2;
    }
    au_docview_stats(dv, &s1);
    AuBuf msg;
    au_buf_init(&msg);
    uint32_t bad = au_docview_check_failures(dv, &msg);
    CHECK(bad == 0, "%s: %u differences from a whole build: %s", c->name, bad, au_buf_cstr(&msg));
    uint32_t builds = s1.tree_builds - s0.tree_builds, subs = s1.subtree_builds - s0.subtree_builds;
    if (c->expect == INCR)
        CHECK(builds == 0, "%s: %u whole builds, expected none (%u subtree rebuilds)", c->name, builds, subs);
    if (c->expect == FALLBACK)
        CHECK(builds > 0, "%s: no whole build, expected one", c->name);
    if (verbose)
        printf("%-40s %u whole builds, %u subtree rebuilds (%u boxes), %u elements styled\n", c->name, builds, subs,
               s1.subtree_boxes - s0.subtree_boxes, s1.styled_elements - s0.styled_elements);
    au_buf_free(&msg);
    au_buf_free(&step);
    au_buf_free(&out);
    au_buf_free(&html);
    au_docview_free(dv);
}

/* A keystroke's worth of suggestions restyles only the list and its
 * neighbours: not the article. */
static void test_restyle_scope(void)
{
    AuDocView *dv = au_docview_new(fonts, NULL, NULL);
    au_docview_set_scripting(dv, true);
    au_docview_set_viewport(dv, 400, 300);
    AuBuf html;
    au_buf_init(&html);
    au_buf_puts(&html, "<!DOCTYPE html><style>body{font:10px Ahem} .m li:first-child{color:red} "
                       ".m:empty{display:none} p:nth-child(2n){color:blue}</style><div><ul class=m id=m></ul></div>");
    for (int i = 0; i < 300; i++)
        au_buf_printf(&html, "<p>paragraph %d <a href=#>link</a></p>", i);
    au_docview_load(dv, html.data, html.len, "http://example.com/");
    au_docview_update(dv);
    AuDocViewStats s0, s1;
    au_docview_stats(dv, &s0);
    AuBuf out;
    au_buf_init(&out);
    au_docview_eval(dv,
                    "var m=document.getElementById('m');for(var i=0;i<5;i++){var l=document.createElement('li');l.textContent='s'+i;m.appendChild(l)};''",
                    &out);
    au_docview_update(dv);
    au_docview_stats(dv, &s1);
    uint32_t styled = s1.styled_elements - s0.styled_elements;
    CHECK(styled <= 12, "suggestions restyled %u elements", styled);
    CHECK(s1.tree_builds == s0.tree_builds, "suggestions built the whole tree");
    CHECK(s1.subtree_builds > s0.subtree_builds, "suggestions rebuilt no subtree");
    CHECK(s1.subtree_boxes - s0.subtree_boxes < 40, "suggestions built %u boxes",
          s1.subtree_boxes - s0.subtree_boxes);
    au_buf_free(&out);
    au_buf_free(&html);
    au_docview_free(dv);
}

/* A title set by script reaches the view without a whole build. */
static void test_title(void)
{
    AuDocView *dv = au_docview_new(fonts, NULL, NULL);
    au_docview_set_scripting(dv, true);
    au_docview_set_viewport(dv, 400, 300);
    const char *html = "<!DOCTYPE html><title>one</title><p>text</p>";
    au_docview_load(dv, html, strlen(html), "http://example.com/");
    au_docview_update(dv);
    AuBuf out;
    au_buf_init(&out);
    au_docview_eval(dv, "document.title += ' two';''", &out);
    au_docview_update(dv);
    CHECK(!strcmp(au_docview_title(dv), "one two"), "the title is '%s'", au_docview_title(dv));
    au_buf_free(&out);
    au_docview_free(dv);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    static uint8_t fontdata[16384];
    fonts = ahem_fontset(fontdata);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        run_case(&cases[i]);
    test_restyle_scope();
    test_title();
    au_fontset_free(fonts);
    printf("incremental: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
