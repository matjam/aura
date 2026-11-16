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
 * Scripts also install listeners on the events the view fires while it
 * handles input (focus, blur, mousedown, input, change, submit, ...)
 * that destroy the target, its form, an ancestor, the document or the
 * frame; start workers that post, end, throw or never return, and
 * terminate them mid-message; draw on canvases that are then removed;
 * and insert, navigate and remove iframes whose documents remove their
 * own frame. Between steps the input is aimed at the elements the page
 * shows (presses with and without a pointer move first, drags, editing
 * keys, the Edit commands), and now and then the view is hidden and
 * shown again, which releases its layout and builds it again while
 * script holds references. One run in four collects script garbage
 * whenever script is left (au_js_set_gc_stress).
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
#include "js/js.h"
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
    "<div id=dh><template shadowrootmode=open><style>:host{display:block}::slotted(*){color:blue}</style>"
    "<slot name=a></slot><b part=x>sh</b><slot></slot></template><i slot=a>slotted</i>light</div>"
    "<script>var E=[],R=[],T=[],L=[],O=[],W=[],C=[],CS=[],n=0;"
    "function sr(i){var x=e(i);return x&&x.shadowRoot||document}"
    "function cs(i){if(!CS.length)CS.push(new CSSStyleSheet());return CS[i%CS.length]}"
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
                                    "data-x", "aria-label", "onclick", "media", "rel", "align", "bgcolor", "part",
                                    "exportparts"};
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
    "<slot name=a>fb</slot><slot></slot>", "<span slot=a>sa</span><b part=x exportparts=x:y>p</b>",
    "<div><template shadowrootmode=open><slot></slot><p part=x>d</p></template>dl</div>",
};
/* What shadow trees hold: slots, styles with the scoping selectors. */
static const char *const shadow_html[] = {
    "<slot></slot>", "<slot name=a></slot><slot></slot>", "<p>no slot</p>",
    "<style>:host{display:flex}::slotted(*){display:block;width:10px}</style><slot></slot>",
    "<style>:host(.c){color:red}:host-context(ul) p{display:none}p{float:left}</style><p part=x>p</p><slot name=a>fb</slot>",
    "<div><slot></slot></div><span part=\"x y\" exportparts=\"x\">s</span><slot name=a></slot>",
    "<link rel=stylesheet href=x.css><slot></slot><x-el></x-el>",
    "<table><tr><td><slot></slot></td></tr></table>", "<ul><li><slot name=a></slot></li></ul><input>",
};
static const char *const scoped_css[] = {
    ":host{display:block;padding:1px}", "::slotted(*){float:left}", "::slotted(span){display:none}",
    ":host(.c) slot{display:block}", "p{display:inline-block;width:5px}", ":host-context(div){color:red}",
    "slot{display:block}", "*{display:contents}", "@media (min-width:1px){:host{display:none}}",
    "::part(x){display:table}", "#a::part(y){width:1e9px}", "b{position:absolute}",
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

/* ---- Listeners that destroy things during input ----
 * The events the view fires from C while it handles the mouse, the
 * keyboard and form controls, and what a listener for one does: remove
 * the target, its form, its parent or the frame, empty a control (which
 * frees the element under the pointer), write the document again,
 * navigate, move the focus. Each ends with a forced layout, since the
 * box tree holds the nodes it shows until it is built again. Domato
 * found a use of a freed element this way that this fuzzer did not,
 * because its listeners were never on these events. The strings that
 * go inside a quoted script string (controls, worker_src, frame_docs)
 * hold no single quote. */
static const char *const input_events[] = {"focus",    "blur",     "focusin",  "focusout", "mousedown",   "mouseup", "click",
                                           "mousemove", "mouseover", "mouseout", "keydown",  "keypress",    "keyup",   "beforeinput",
                                           "input",    "change",   "submit",   "invalid",  "reset",       "select",  "toggle"};
static const char *const destroy[] = {
    "ev.target.remove()",
    "ev.target.form.remove()",
    "ev.target.parentNode.remove()",
    "ev.target.innerHTML=\"\"",
    "ev.target.textContent=\"\"",
    "document.activeElement.innerHTML=\"\"",
    "document.activeElement.remove()",
    "document.body.innerHTML=\"<input autofocus><p>x</p>\"",
    "document.open();document.write(\"<p>w</p><input>\");document.close()",
    "document.documentElement.remove()",
    "document.implementation.createHTMLDocument(\"\").body.appendChild(ev.target)",
    "location.href=\"#h\"+n",
    "location.href=\"http://fuzz.example/next\"",
    "if(window.frameElement)frameElement.remove()",
    "ev.target.parentNode.replaceChildren()",
    "document.querySelectorAll(\"iframe,canvas,select,input,button,a,b,i\").forEach(function(x){x.remove()})",
    "ev.target.focus()",
    "ev.target.blur()",
    "ev.target.form.submit()",
    "ev.target.form.reset()",
    "ev.target.click()",
    "ev.target.disabled=true",
    "ev.target.type=\"checkbox\"",
    "ev.target.closest(\"form,div,body\").innerHTML=\"<b>gone</b>\"",
    "ev.preventDefault()",
};
/* Controls whose text is an element of its own (the hit test names the
 * inner element, the control gets the focus) and the other kinds of
 * control, to give the input between steps something to act on. */
static const char *const controls[] = {
    "<button><b>go</b></button>",
    "<a href=\"#x\"><i>link</i></a>",
    "<label>l <span>s</span><input></label>",
    "<details><summary><b>s</b></summary>d</details>",
    "<form action=/s><input required><input value=v><button><i>b</i></button><input type=reset></form>",
    "<form><input name=q><input type=submit></form>",
    "<select size=3 multiple><option>a<option>b<option>c</select>",
    "<select><option>x<option>y<option>z</select>",
    "<input type=range><input type=checkbox><input type=radio name=r><input type=radio name=r>",
    "<textarea>some text here</textarea>",
    "<span tabindex=0><i>t</i></span>",
    "<input type=file><input type=search value=s><input type=password value=p>",
};
/* Worker scripts: one that answers, ends itself, throws, imports a
 * script that does not load, never returns, posts on a timer. */
static const char *const worker_src[] = {
    "onmessage=function(e){postMessage(e.data)}",
    "postMessage(1);close()",
    "throw new Error(\"x\")",
    "importScripts(\"http://fuzz.example/none.js\")",
    "onmessage=function(e){for(;;);}",
    "setInterval(function(){postMessage(Date.now())},1)",
    "onmessage=function(e){postMessage({a:[1,2,{b:e.data}],c:new ArrayBuffer(64)})};postMessage(0)",
    "self.close();postMessage(2)",
    "importScripts(\"data:text/javascript,postMessage(3)\");onmessage=function(){close()}",
    "onmessage=function(e){throw e.data}",
};
static const char *const worker_ops[] = {
    "w.postMessage({k:n,a:new ArrayBuffer(8)})",
    "w.postMessage(1);w.terminate()",
    "w.terminate();w.postMessage(2)",
    "W.splice(0,1);w.terminate()",
    "var b=new ArrayBuffer(16);w.postMessage(b,[b])",
    "w.onmessage=null;w.postMessage(3)",
};
static const char *const canvas_ops[] = {
    "x.fillRect(0,0,1e9,1e9)",
    "x.drawImage(x.canvas,0,0)",
    "x.canvas.remove();x.fillText(\"t\",1,9)",
    "x.canvas.width=0;x.fillRect(0,0,1,1)",
    "x.canvas.width=33;x.strokeRect(1,1,5,5)",
    "x.getImageData(0,0,4,4)",
    "x.putImageData(x.getImageData(0,0,2,2),1e9,-1e9)",
    "x.save();x.translate(1e30,0);x.arc(0,0,5,0,7);x.fill();x.restore()",
    "x.createPattern(x.canvas,\"repeat\")",
    "x.canvas.toDataURL()",
    "x.font=\"20px a\";x.strokeText(\"abc\",NaN,1)",
    "x.clip();x.clearRect(-1,-1,1e9,1e9)",
    "x.setTransform(1e20,0,0,1e-20,0,0);x.fillRect(0,0,1,1)",
    "x.canvas.style.display=\"none\";x.fillRect(0,0,9,9);x.canvas.parentNode.removeChild(x.canvas)",
    "x.beginPath();x.moveTo(0,0);x.lineTo(1e38,NaN);x.stroke()",
    "x.shadowBlur=1e9;x.shadowColor=\"red\";x.fillRect(1,1,3,3)",
};
/* Documents for iframes (srcdoc): some remove their own frame, or
 * change the embedding document, from a script or a listener. */
static const char *const frame_docs[] = {
    "<p>f</p>",
    "<input autofocus><button><b>b</b></button>",
    "<script>parent.n++;frameElement.remove()</script>",
    "<script>onfocus=function(){frameElement.remove()};document.onmousedown=function(){parent.document.body.innerHTML=1}</script><input>",
    "<iframe srcdoc=\"<p>in</p><input>\"></iframe><p>outer</p>",
    "<script>setTimeout(function(){frameElement.remove()},1)</script><p>t</p>",
    "<body onload=\"parent.document.querySelector(&quot;iframe&quot;).src=&quot;about:blank&quot;\">x",
    ("<form><input required><button>go</button></form><script>document.forms[0].oninvalid=function(){frameElement.remove()};"
     "document.onkeydown=function(){frameElement.parentNode.removeChild(frameElement)}</script>"),
    "<script>document.onmouseover=document.onclick=function(e){parent.document.documentElement.appendChild(e.target)}</script><a href=#>a</a>",
};
static const char *const frame_ops[] = {
    "f.src=\"about:blank\"",
    "f.srcdoc=\"<p>new</p><input autofocus>\"",
    "f.contentWindow.location.href=\"about:blank\"",
    "add(f.contentDocument.body);add(f.contentDocument.documentElement)",
    "f.remove()",
    "f.contentWindow.eval('parent.document.querySelector(\"iframe\").remove()')",
    "f.contentDocument.open();f.contentDocument.write(\"<input autofocus>\");f.contentDocument.close()",
    "f.contentWindow.focus()",
    "f.contentDocument.querySelector(\"input\").focus()",
    "document.body.appendChild(f)",
    "f.contentWindow.postMessage(1,\"*\")",
    "f.style.display=\"none\"",
    "f.width=7;f.height=1e9",
    "f.contentDocument.body.appendChild(document.getElementById(\"i1\"))",
};

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
    uint32_t op = fz_rnd(depth > 1 ? 40 : 94);
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
    case 72:
    case 73:
    case 74: {
        /* a listener on an input event that destroys something */
        static const char *const opts[] = {"true", "false", "{capture:true,once:true}", "{once:true}"};
        int k, nd = 1 + (int)fz_rnd(2);
        fz_puts(b, "var h=function(ev){");
        for (k = 0; k < nd; k++)
            put(b, "try{%s}catch(_){}", PICK(destroy));
        if (fz_rnd(4))
            fz_puts(b, "try{(document.body||document.documentElement).offsetWidth}catch(_){}");
        fz_puts(b, "};L.push(h);");
        if (fz_rnd(3))
            put(b, "document.addEventListener('%s',h,%s)", PICK(input_events), PICK(opts));
        else
            put(b, "e(%u).addEventListener('%s',h,%s)", N(), PICK(input_events), PICK(opts));
        break;
    }
    case 75:
    case 76: {
        static const char *const where[] = {"beforebegin", "afterbegin", "beforeend", "afterend"};
        put(b, "(%s).insertAdjacentHTML('%s','%s');all()", fz_rnd(3) ? "document.body" : "e(0)", PICK(where), PICK(controls));
        break;
    }
    case 77:
        put(b, "if(W.length<3){var w=new Worker(URL.createObjectURL(new Blob(['%s'])));W.push(w);w.onmessage=", PICK(worker_src));
        gen_callback(b, depth);
        fz_puts(b, ";w.onerror=");
        gen_callback(b, depth);
        put(b, ";w.postMessage(%u)}", N());
        break;
    case 78: put(b, "var w=W[%u%%(W.length||1)];%s", N(), PICK(worker_ops)); break;
    case 79:
        put(b, "var c=document.createElement('canvas');c.width=%u;c.height=%u;add(c);e(%u).appendChild(c);"
               "var x=c.getContext('2d');C.push(x);x.fillStyle='red';x.fillRect(1,1,%u,%u)",
            fz_rnd(64), fz_rnd(64), N(), fz_rnd(40), fz_rnd(40));
        break;
    case 80: put(b, "var x=C[%u%%(C.length||1)];%s;x.drawImage(e(%u),0,0)", N(), PICK(canvas_ops), N()); break;
    case 81:
        put(b, "var f=document.createElement('iframe');f.srcdoc='%s';add(f);f.onload=", PICK(frame_docs));
        gen_callback(b, depth);
        put(b, ";(%s).appendChild(f)", fz_rnd(2) ? "document.body" : "e(1)");
        break;
    case 82:
    case 83: put(b, "var f=document.querySelectorAll('iframe')[%u%%(document.querySelectorAll('iframe').length||1)];%s", fz_rnd(4), PICK(frame_ops)); break;
    /* ---- shadow trees: attach, slots, scoped and adopted sheets ---- */
    case 84:
        put(b, "var x=e(%u).attachShadow({mode:'%s',slotAssignment:'%s',delegatesFocus:%s});add(x);x.innerHTML='%s'",
            N(), fz_rnd(2) ? "open" : "closed", fz_rnd(3) ? "named" : "manual", fz_rnd(2) ? "true" : "false", PICK(shadow_html));
        break;
    case 85: put(b, "var s=sr(%u).querySelector('slot');if(s){s.assign(e(%u),t(%u));add(s)}", N(), N(), N()); break;
    case 86: put(b, "var s=sr(%u).querySelectorAll('slot');for(var q of s){add(q);q.assignedNodes({flatten:true})}", N()); break;
    case 87:
        put(b, "var c=new CSSStyleSheet();c.replaceSync('%s %s');if(CS.length<8)CS.push(c);sr(%u).adoptedStyleSheets=[c,cs(%u)]",
            PICK(scoped_css), PICK(scoped_css), N(), N());
        break;
    case 88: {
        static const char *const sheet_ops[] = {
            "insertRule('%s',0)", "replaceSync('%s')", "disabled=!x.disabled;x.replace('%s')", "media.mediaText='print';x.insertRule('%s')",
            "cssRules.length&&x.deleteRule(0);'%s'",
        };
        put(b, "var x=cs(%u);x.", N());
        put(b, PICK(sheet_ops), PICK(scoped_css));
        break;
    }
    case 89:
        put(b, "var a=sr(%u).adoptedStyleSheets;%s", N(),
            fz_rnd(2) ? "a.push(cs(1))" : fz_rnd(2) ? "a.pop()" : "a.splice(0,1,cs(2))");
        break;
    case 90: put(b, "e(%u).setHTMLUnsafe('<div><template shadowrootmode=open>%s</template>x</div>');all()", N(), PICK(shadow_html)); break;
    case 91: put(b, "e(%u).setAttribute('%s','%s')", N(), fz_rnd(2) ? "slot" : fz_rnd(2) ? "name" : "part", fz_rnd(2) ? "a" : "x"); break;
    case 92: put(b, "var r=sr(%u);for(var q of r.querySelectorAll('*'))add(q);add(r.firstChild)", N()); break;
    case 93:
        put(b, "add(document.elementFromPoint(%u,%u));document.activeElement;sr(%u).activeElement", fz_rnd(300), fz_rnd(300), N());
        break;
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

/* The embedder's menu for a select and its file dialog: a choice is
 * made, so the view goes on to fire input and change. */
static int popup_cb(void *ctx, AuDocView *dv, const AuPopupItem *items, int n, int selected, AuIRect at)
{
    (void)ctx, (void)dv, (void)items, (void)at;
    return n > 1 ? (selected + 1) % n : -1;
}

static bool choose_file_cb(void *ctx, AuDocView *dv, bool multiple, AuControlFile **files, uint32_t *n)
{
    AuControlFile *f = au_calloc(sizeof *f);
    (void)ctx, (void)dv, (void)multiple;
    f->name = au_strndup("a.txt", 5);
    f->type = au_strndup("text/plain", 10);
    f->data = au_strndup("data", 4);
    f->len = 4;
    *files = f;
    *n = 1;
    return true;
}

static double fake_now;
/* The clock script sees: CPU time, twenty times as fast, so that the
 * engine's ten-second limit on a script that does not end (callbacks
 * that keep each other going) costs half a second here. */
static double now_ms(void *ctx, AuDocView *dv)
{
    (void)ctx, (void)dv;
    fake_now += 0.001;
    return fake_now + (double)clock() * 20000.0 / CLOCKS_PER_SEC;
}

/* The centre of a random element that has a box, a form control or
 * something inside one three times in four when there is one; false
 * when the document shows nothing. */
static bool random_target(AuDocView *dv, int *x, int *y)
{
    AuDocument *d;
    AuNode *n, *pick = NULL, *pick_ctl = NULL;
    uint32_t seen = 0, seen_ctl = 0;
    AuRect r;
    au_docview_update(dv);
    d = au_docview_document(dv);
    if (!d)
        return false;
    for (n = au_node_next(&d->node, &d->node); n; n = au_node_next(n, &d->node)) {
        AuNode *a;
        bool ctl = false;
        if (n->type != AU_NODE_ELEMENT || !au_layout_element_rect(au_docview_layout(dv), au_as_element(n), &r) ||
            !(r.w > 0 && r.h > 0))
            continue;
        for (a = n; a && !ctl; a = a->parent) {
            AuAtom t = a->type == AU_NODE_ELEMENT ? au_as_element(a)->tag : 0;
            ctl = t == AU_ATOM_input || t == AU_ATOM_select || t == AU_ATOM_textarea || t == AU_ATOM_button ||
                  t == AU_ATOM_a || t == AU_ATOM_label || t == AU_ATOM_summary || t == AU_ATOM_iframe;
        }
        /* reservoir sampling: each candidate is as likely */
        if (fz_rnd(++seen) == 0)
            pick = n;
        if (ctl && fz_rnd(++seen_ctl) == 0)
            pick_ctl = n;
    }
    if (pick_ctl && fz_rnd(4))
        pick = pick_ctl;
    if (!pick || !au_layout_element_rect(au_docview_layout(dv), au_as_element(pick), &r))
        return false;
    /* clamped: a page puts boxes anywhere */
    *x = au_f2px(r.x + r.w / 2);
    *y = au_f2px(r.y + r.h / 2);
    return true;
}

static void mouse_at(AuDocView *dv, int type, int x, int y, int clicks, uint32_t mods)
{
    AuEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.x = x;
    ev.y = y;
    ev.clicks = clicks;
    ev.mods = mods;
    au_docview_mouse(dv, &ev);
}

/* Input aimed at what the page shows: a press on an element (with or
 * without the pointer moving there first, a drag, a double click, with
 * modifiers), editing keys, and the Edit commands. The listeners of
 * case 72 destroy things while the view handles these. */
static void more_input(AuDocView *dv)
{
    static const int keys[] = {AU_KEY_TAB,  AU_KEY_RETURN, AU_KEY_BACKSPACE, AU_KEY_DELETE, AU_KEY_LEFT, AU_KEY_RIGHT,
                               AU_KEY_UP,   AU_KEY_DOWN,   AU_KEY_HOME,      AU_KEY_END,    AU_KEY_ESCAPE, AU_KEY_PAGE_DOWN};
    static const uint32_t modsv[] = {0, 0, 0, AU_MOD_SHIFT, AU_MOD_COMMAND, AU_MOD_OPTION};
    AuEvent ev;
    int x, y, k, nk;
    if (fz_rnd(3) && random_target(dv, &x, &y)) {
        int clicks = fz_rnd(5) ? 1 : 2 + (int)fz_rnd(2);
        uint32_t mods = PICK(modsv);
        if (fz_rnd(2))
            mouse_at(dv, AU_EVENT_MOUSE_MOVE, x, y, 0, 0);
        mouse_at(dv, AU_EVENT_MOUSE_DOWN, x, y, clicks, mods);
        if (fz_rnd(3) == 0)
            mouse_at(dv, AU_EVENT_MOUSE_MOVE, x + (int)fz_rnd(30) - 10, y + (int)fz_rnd(20) - 5, 0, 0);
        mouse_at(dv, AU_EVENT_MOUSE_UP, x, y, clicks, mods);
    }
    nk = (int)fz_rnd(4);
    for (k = 0; k < nk; k++) {
        memset(&ev, 0, sizeof ev);
        ev.type = AU_EVENT_KEY_DOWN;
        if (fz_rnd(3))
            ev.key = PICK(keys);
        else
            ev.ch = fz_rnd(3) ? 'a' + fz_rnd(26) : ' ';
        ev.mods = PICK(modsv);
        au_docview_key(dv, &ev);
    }
    if (fz_rnd(4) == 0) {
        AuDocView *v = au_docview_focus_view(dv);
        if (v && au_docview_editing(v)) {
            static const int cmds[] = {AU_EDITCMD_UNDO, AU_EDITCMD_CUT, AU_EDITCMD_COPY, AU_EDITCMD_PASTE, AU_EDITCMD_CLEAR,
                                       AU_EDITCMD_SELECT_ALL};
            AuBuf out;
            au_buf_init(&out);
            au_docview_edit(v, PICK(cmds), "paste", 5, &out);
            au_buf_free(&out);
        }
    }
    if (fz_rnd(8) == 0)
        au_docview_set_focus(dv, NULL);
}

/* The document is hidden (a kept document of another history entry, a
 * background tab): its layout is released. Script may run meanwhile; the
 * layout is built again when it is shown. */
static void hide_and_show(AuDocView *dv)
{
    AuCanvas *c;
    AuHitResult hr;
    au_docview_set_hidden(dv, true);
    if (fz_rnd(2))
        au_docview_run_scripts(dv, 5);
    if (fz_rnd(3) == 0) {
        AuBuf out;
        au_buf_init(&out);
        au_docview_eval(dv, "try{document.body.offsetWidth;getSelection().toString();e(n).getBoundingClientRect()}catch(_){}0", &out);
        au_buf_free(&out);
    }
    au_docview_set_hidden(dv, false);
    if (fz_rnd(2))
        au_docview_run_scripts(dv, 5);
    au_docview_update(dv);
    c = au_canvas_new(surface);
    au_docview_paint(dv, c, (AuRect){0, 0, 200, 150}, 0, 0);
    au_canvas_free(c);
    au_docview_hit_test(dv, (int)fz_rnd(300), (int)fz_rnd(200), &hr);
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
    more_input(dv);
    for (m = 0; m < 4; m++)
        au_docview_run_scripts(dv, 5);
    au_docview_update(dv);
    if (fz_rnd(6) == 0)
        hide_and_show(dv);
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
    cb.popup = popup_cb;
    cb.choose_file = choose_file_cb;
    fake_now = 1000;
    /* One run in four collects script garbage whenever script is left
     * (slow): a node that only garbage names is then freed as soon as it
     * is removed. Chosen from the state without drawing a number, so the
     * saved scripts replay as before. */
    au_js_set_gc_stress(((fz_state >> 9) & 3) == 0);
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
