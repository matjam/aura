/* JSTest: the JavaScript engine on Mac OS 9, without the browser.
 *
 * Starts a runtime against an empty document, runs a self-test of the
 * engine and the DOM bindings, then the benchmarks of
 * tests/js/bench/bench.js (embedded) at BENCH_SCALE 0.1, and shows every
 * line in its window: start-up time and memory, PASS/FAIL lines, "bench
 * NAME MS" lines and the heap left. Click or press a key to quit.
 *
 * To compare with the host, run `aura-host js --stats SCALE.js
 * tests/js/bench/bench.js` where SCALE.js sets globalThis.BENCH_SCALE =
 * 0.1. */
#include <Multiverse.h>

#include "html/html.h"
#include "js/js.h"

#include <stdio.h>
#include <string.h>

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wc23-extensions"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

static const char jt_bench[] = {
#embed "../../../../tests/js/bench/bench.js"
    , 0};

/* The self-test: engine features that depend on the platform (byte
 * order, libm, dates, the allocator) and a pass over the DOM bindings.
 * Each runs as its own script, so one that is stopped (out of memory, say)
 * does not hide the others; each prints "ok NAME" or "FAIL NAME". */
static const char *const jt_selftest[] = {
    "arith", "(0.1+0.2).toFixed(2)==='0.30'&&(2**53+1)===2**53&&(7/2|0)===3",
    "loop", "(()=>{let s=0;for(let i=0,m=200000;i<m;i++)s=(s+i*3)|0;return s===-129842144||s;})()",
    "loop-count", "(()=>{let c=0;for(let i=0;i<1000;i++)c++;return c===1000||c;})()",
    "new", "(()=>{class P{constructor(x){this.x=x;}}return new P(5).x===5;})()",
    "method", "(()=>{const o={v:3,f(){return this.v*2;}};return o.f()===6;})()",
    "class-method", "(()=>{class P{constructor(x,y){this.x=x;this.y=y;}len(){return this.x*this.x+this.y*this.y;}}"
                    "return new P(3,4).len()===25;})()",
    "props-loop", "(()=>{class P{constructor(x,y){this.x=x;this.y=y;}len(){return this.x*this.x+this.y*this.y;}}"
                  "let q=0;for(let i=0;i<1000;i++)q+=new P(i,i+1).len()&255;return q===127480||q;})()",
    "for-of-array", "(()=>{let c=0;for(const x of [1,2,3])c+=x;return c===6;})()",
    "string-iter", "(()=>{const it='ab'[Symbol.iterator]();const r=[it.next(),it.next(),it.next()];"
                   "return JSON.stringify(r)==='[{\"value\":\"a\",\"done\":false},{\"value\":\"b\",\"done\":false},"
                   "{\"done\":true}]'||JSON.stringify(r);})()",
    "alloc-many", "(()=>{let k=0;for(let i=0;i<20000;i++){const o={i,s:'x'+i};o.self=o;k+=o.i&1;}return k===10000||k;})()",
    "math", "Math.abs(Math.cbrt(27)-3)<1e-12&&Math.abs(Math.hypot(3,4)-5)<1e-12&&Math.round(Math.sinh(1)*1e6)===1175201",
    "dtoa", "String(1e21)==='1e+21'&&String(123.456)==='123.456'&&parseFloat('3.25e2')===325",
    "endian", "(()=>{const b=new ArrayBuffer(4);new DataView(b).setUint32(0,0x11223344);return new Uint8Array(b)[0]===0x11;})()",
    "bigint", "(2n**64n).toString()==='18446744073709551616'",
    "regexp", "/(\\d+)-(\\d+)/.exec('a 12-34')[2]==='34'&&'aBc'.replace(/b/i,'x')==='axc'",
    "regexp-unicode", "/\\p{L}+/u.test('\\u00e9t\\u00e9')",
    "json", "JSON.stringify(JSON.parse('{\"a\":[1,2,{\"b\":null}]}'))==='{\"a\":[1,2,{\"b\":null}]}'",
    "case", "'\\u00c9cole'.toLowerCase()==='\\u00e9cole'&&'stra\\u00dfe'.toUpperCase()==='STRASSE'",
    "astral", "(()=>{const a=[...'a\\u{1f600}'];return a.length===2||a.map((c)=>c.length+':'+c.codePointAt(0)).join();})()",
    "fromcodepoint", "String.fromCodePoint(0x1f600).length===2&&'\\u{1f600}'.codePointAt(0)===0x1f600",
    "date", "new Date(Date.UTC(2000,0,2)).toISOString()==='2000-01-02T00:00:00.000Z'",
    "collections", "(()=>{const m=new Map([[1,'a']]);const w=new WeakMap();const k={};w.set(k,1);"
                   "return m.get(1)==='a'&&w.get(k)===1&&new Set([1,1,2]).size===2;})()",
    "classes", "(()=>{class A{#x=1;get x(){return this.#x;}}class B extends A{}return new B().x===1;})()",
    "generators", "(()=>{function*g(){yield 1;yield 2;}return [...g()].join()==='1,2';})()",
    "dom tree", "(()=>{const d=document.createElement('div');d.innerHTML='<p id=a class=x>hi <b>there</b></p>';"
                "return d.querySelector('#a b').textContent==='there'&&d.firstChild.className==='x';})()",
    "dom events", "(()=>{let c=0;const e=document.createElement('i');document.body.appendChild(e);"
                  "document.body.addEventListener('x',()=>c++);e.dispatchEvent(new Event('x',{bubbles:true}));"
                  "e.remove();return c===1;})()",
    /* regular expression literals, numbers and strings inside the prelude,
     * which is loaded from bytecode written on a little-endian host */
    "prelude regexp", "(()=>{const d=document.createElement('div');d.dataset.fooBar='1';d.className=' a  b ';"
                      "const r=[d.getAttribute('data-foo-bar')==='1',d.classList.length===2,"
                      "CSS.escape('a b')==='a\\\\ b',new URLSearchParams('a=1&b=%20x').get('b')===' x'];"
                      "return r.every((x)=>x)||r.join()+' '+d.getAttribute('data-foo-bar')+' '+d.classList.length+' '+"
                      "CSS.escape('a b')+' ['+new URLSearchParams('a=1&b=%20x').get('b')+']';})()",
    "prelude forms", "(()=>{const d=document.createElement('div');d.style.cssText='color: red; margin-top: 2px';"
                     "d.innerHTML='<input type=email value=\"a@b.c\">';return d.style.marginTop==='2px'&&"
                     "d.firstChild.validity.valid===true&&d.firstChild.checkValidity();})()",
    "url", "new URL('../b?c=1#d','http://h.example/a/x').href==='http://h.example/b?c=1#d'",
    "textencoder", "new TextEncoder().encode('\\u00e9').length===2&&"
                   "new TextDecoder().decode(new Uint8Array([0xc3,0xa9]))==='\\u00e9'",
    "promises", "(()=>{let r=false;Promise.resolve().then(()=>{r=true;});return true;})()",
    /* the frame's var_refs at the top of an alloca block (patch 0004) */
    "closures", "(()=>{globalThis.__S=0.25;globalThis.__I=7;const r=["
                "(()=>{const a=globalThis.__S;const f=()=>a;return f();})(),"
                "(()=>{const a=globalThis.__I;const f=()=>a;return (0,f)();})(),"
                "(()=>{const a=__S;const f=()=>a;f();return f();})(),"
                "(()=>{const p=globalThis.__S;return ((k)=>k*p)(4);})()];"
                "return r.join()==='0.25,7,0.25,1'||r.join();})()",
    "performance.now", "(()=>{const a=performance.now();const b=performance.now();return typeof a==='number'&&b>=a;})()",
    "long script", "(()=>{const t0=Date.now();let n=0;while(Date.now()-t0<3000)n++;return n>0;})()",
    /* last: a failure here may take the application down */
    "stack overflow", "(()=>{let d=0;const f=()=>{d++;f();};try{f();}catch(e){globalThis.__depth=d;"
                      "return e instanceof RangeError&&d>50||d;}return false;})()",
};

typedef struct {
    uint32_t hi, lo;
} JtWide;
/* Microseconds is in InterfaceLib; the Multiversal headers leave it out. */
extern pascal void Microseconds(JtWide *us);

#define JT_LINES 40
#define JT_COLS 96

typedef struct Jt {
    WindowPtr win;
    char lines[JT_LINES][JT_COLS];
    int nlines;
    int errors;
    uint32_t limit_ms; /* the current script's time limit */
} Jt;

static bool jt_long_script(void *ctx, uint32_t elapsed_ms)
{
    Jt *t = ctx;
    return elapsed_ms < t->limit_ms;
}

static double jt_now(void *ctx)
{
    (void)ctx;
    JtWide us;
    Microseconds(&us);
    return ((double)us.hi * 4294967296.0 + (double)us.lo) / 1000.0;
}

static void jt_draw(Jt *t)
{
    SetPort(t->win);
    Rect r = t->win->portRect;
    EraseRect(&r);
    TextFont(4); /* Monaco */
    TextSize(9);
    for (int i = 0; i < t->nlines; i++) {
        Str255 p;
        size_t n = strlen(t->lines[i]);
        p[0] = (unsigned char)n;
        memcpy(p + 1, t->lines[i], n);
        MoveTo(6, 11 + i * 10);
        DrawString(p);
    }
}

static void jt_line(Jt *t, const char *s, size_t len)
{
    if (t->nlines == JT_LINES) {
        memmove(t->lines[0], t->lines[1], sizeof t->lines[0] * (JT_LINES - 1));
        t->nlines--;
    }
    if (len >= JT_COLS)
        len = JT_COLS - 1;
    memcpy(t->lines[t->nlines], s, len);
    t->lines[t->nlines][len] = 0;
    t->nlines++;
    jt_draw(t);
}

static void jt_printf(Jt *t, const char *s)
{
    jt_line(t, s, strlen(s));
}

/* A progress line. With JT_STEP_TICKS > 0 each one stays on screen that
 * long, so a crash can be placed from screenshots. */
#ifndef JT_STEP_TICKS
#define JT_STEP_TICKS 0
#endif
static void jt_step(Jt *t, const char *s)
{
    jt_printf(t, s);
    if (JT_STEP_TICKS) {
        LONGINT end;
        Delay(JT_STEP_TICKS, &end);
    }
}

static void jt_console(void *ctx, AuJsLogLevel level, const char *text, size_t len)
{
    Jt *t = ctx;
    if (level == AU_JS_ERROR)
        t->errors++;
    /* one window line per text line */
    while (len) {
        const char *nl = memchr(text, '\n', len);
        size_t n = nl ? (size_t)(nl - text) : len;
        jt_line(t, text, n);
        if (!nl)
            break;
        len -= n + 1;
        text = nl + 1;
    }
}

/* Runs self-test i: its expression must give true without an error. */
static void jt_check(Jt *t, AuJs *js, size_t i, int *pass, int *fail)
{
    AuBuf src, out;
    au_buf_init(&src);
    au_buf_init(&out);
    au_buf_printf(&src, "String(%s)", jt_selftest[2 * i + 1]);
    int before = t->errors;
    bool ok = au_js_eval(js, src.data, src.len, &out) && out.len == 4 && !memcmp(out.data, "true", 4) &&
              t->errors == before;
    char buf[160];
    snprintf(buf, sizeof buf, "%s %s%s%.80s", ok ? "ok" : "FAIL", jt_selftest[2 * i], ok ? "" : ": ",
             ok ? "" : au_buf_cstr(&out));
    jt_step(t, buf);
    if (ok)
        (*pass)++;
    else
        (*fail)++;
    au_buf_free(&src);
    au_buf_free(&out);
}

static void jt_wait(Jt *t)
{
    for (;;) {
        EventRecord ev;
        if (!WaitNextEvent(everyEvent, &ev, 30, NULL))
            continue;
        if (ev.what == mouseDown || ev.what == keyDown)
            return;
        if (ev.what == updateEvt && (WindowPtr)ev.message == t->win) {
            BeginUpdate(t->win);
            jt_draw(t);
            EndUpdate(t->win);
        }
    }
}

int main(void)
{
    /* script may use 512 KB of stack; leave margin for the Toolbox */
    SetApplLimit(GetApplLimit() - 1024 * 1024);
    MaxApplZone();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    Jt t;
    memset(&t, 0, sizeof t);
    Rect wr = {40, 10, 40 + JT_LINES * 10 + 6, 630};
    t.win = NewCWindow(NULL, &wr, (ConstStr255Param) "\pJSTest", true, documentProc, (WindowPtr)-1, false, 0);
    char buf[160];
    jt_step(&t, "Aura JSTest: QuickJS-ng 0.18.0");
    snprintf(buf, sizeof buf, "heap free %ld KB, stack %ld KB", (long)(FreeMem() / 1024),
             (long)(StackSpace() / 1024));
    jt_printf(&t, buf);

    static const char blank[] = "<!DOCTYPE html><html><head><title>t</title></head><body></body></html>";
    AuHtmlOptions o;
    memset(&o, 0, sizeof o);
    o.scripting = true;
    jt_step(&t, "parsing the document");
    double t0 = jt_now(NULL);
    AuDocument *doc = au_html_parse(blank, sizeof blank - 1, &o);
    jt_step(&t, "starting the runtime");
    t0 = jt_now(NULL);
    AuJsSettings s;
    au_js_settings_default(&s);
    s.memory_limit = 16u << 20;
    s.slice_ms = 2000;
    AuJsHooks h;
    memset(&h, 0, sizeof h);
    h.ctx = &t;
    h.now_ms = jt_now;
    h.console = jt_console;
    h.long_script = jt_long_script;
    t.limit_ms = 20000; /* a self-test that loops is stopped */
    AuJs *js = au_js_new(doc, "about:blank", &s, &h);
    double t_start = jt_now(NULL) - t0;
    if (!js) {
        jt_printf(&t, "FAIL cannot start the runtime");
        jt_wait(&t);
        return 1;
    }
    AuJsStats st;
    au_js_stats(js, &st);
    snprintf(buf, sizeof buf, "startup %.1f ms, runtime memory %lu KB", t_start, (unsigned long)(st.memory_used / 1024));
    jt_printf(&t, buf);

    jt_step(&t, "self-test");
    int pass = 0, fail = 0;
    size_t ntests = sizeof jt_selftest / sizeof jt_selftest[0] / 2;
    for (size_t i = 0; i + 1 < ntests; i++)
        jt_check(&t, js, i, &pass, &fail);
    for (int i = 0; i < 100 && !au_js_idle(js); i++)
        au_js_run_tasks(js, 50);
    jt_step(&t, "garbage collection");
    au_js_gc(js);
    jt_step(&t, "collected");

    static const char scale[] = "globalThis.BENCH_SCALE = 0.1;";
    jt_step(&t, "benchmarks");
    au_js_run_classic(js, NULL, scale, sizeof scale - 1, "scale", 1);
    t.limit_ms = 600000; /* the benchmarks are slow on old machines */
    t0 = jt_now(NULL);
    au_js_run_classic(js, NULL, jt_bench, sizeof jt_bench - 1, "bench.js", 1);
    snprintf(buf, sizeof buf, "bench wall %.0f ms", jt_now(NULL) - t0);
    jt_printf(&t, buf);

    t.limit_ms = 20000;
    snprintf(buf, sizeof buf, "stack free %ld KB", (long)(StackSpace() / 1024));
    jt_step(&t, buf);
    jt_check(&t, js, ntests - 1, &pass, &fail);
    {
        AuBuf out;
        au_buf_init(&out);
        static const char q[] = "'recursion depth at the stack limit: ' + globalThis.__depth";
        au_js_eval(js, q, sizeof q - 1, &out);
        jt_step(&t, au_buf_cstr(&out));
        au_buf_free(&out);
    }
    snprintf(buf, sizeof buf, "selftest %d passed, %d failed", pass, fail);
    jt_printf(&t, buf);

    au_js_stats(js, &st);
    snprintf(buf, sizeof buf, "after: runtime memory %lu KB, %lu wrappers", (unsigned long)(st.memory_used / 1024),
             (unsigned long)st.wrappers);
    jt_printf(&t, buf);
    au_js_gc(js);
    au_js_stats(js, &st);
    snprintf(buf, sizeof buf, "after gc: %lu KB; heap free %ld KB; errors %d", (unsigned long)(st.memory_used / 1024),
             (long)(FreeMem() / 1024), t.errors);
    jt_printf(&t, buf);
    au_js_free(js);
    au_node_unref(&doc->node);
    snprintf(buf, sizeof buf, "freed: heap free %ld KB. Click to quit.", (long)(FreeMem() / 1024));
    jt_printf(&t, buf);
    jt_wait(&t);
    return 0;
}
