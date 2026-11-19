function firstLine(o) { var a = 1; return o.missing.x + a; } // keep on line 1
// QuickJS patch 0021 compiles a function at its first call, from the
// script's text, with the variables the function captured when its
// script was compiled (JS_SetLazyFunctions). These cases call functions
// the script compiled as stubs and check that they see the same
// variables, positions and text as functions compiled with the script.
// The line and column numbers below are those of this file: CMakeLists
// runs it with every function compiled at its first call and with none.

function where(fn) {
    try {
        fn();
    } catch (e) {
        const m = /lazy_functions\.js:(\d+):(\d+)/.exec(e.stack);
        return m ? m[1] + ':' + m[2] : e.stack;
    }
    return 'no error';
}

test('a function on the script\'s first line keeps its positions', () => {
    assert.equal(where(() => firstLine({})), '1:43');
});

test('positions in a function compiled at its first call', () => {
    function thrower() {
        let a = 1;
            null.x;
    }
    assert.equal(where(thrower), '27:13');
    const arrow = () => { undefined.y };
    assert.equal(where(arrow), '30:27');
});

test('captured variables of every kind', () => {
    var v = 1;
    let l = 2;
    const c = 3;
    function outer(arg, { d }, [e], ...rest) {
        let shadow = 'outer';
        {
            let shadow = 'block';
            var fromBlock = function () { return shadow; };
        }
        return function inner() {
            return [v, l, c, arg, d, e, rest.length, shadow, fromBlock()];
        };
    }
    assert.deepEqual(outer(4, { d: 5 }, [6], 7, 8)(), [1, 2, 3, 4, 5, 6, 2, 'outer', 'block']);
    l = 20;
    assert.equal(outer(0, { d: 0 }, [0])()[1], 20);
});

test('closures made in a loop from one stub', () => {
    const fns = [];
    for (let i = 0; i < 5; i++)
        fns.push(function () { return i * 10; });
    assert.deepEqual(fns.map((f) => f()), [0, 10, 20, 30, 40]);
    const later = [];
    for (let i = 0; i < 3; i++)
        later.push(() => i);
    // the first call compiles; the others reuse the compiled function
    assert.equal(later[2](), 2);
    assert.equal(later[0](), 0);
    assert.equal(later[1](), 1);
});

test('assignments through captured variables', () => {
    let count = 0;
    function bump(n) { count += n; return count; }
    const add = (n) => { count += n; };
    bump(2);
    add(3);
    assert.equal(count, 5);
    assert.equal(bump(1), 6);
});

test('a function expression sees its own name, a declaration its binding', () => {
    const fact = function f(n) { return n <= 1 ? 1 : n * f(n - 1); };
    assert.equal(fact(5), 120);
    function decl() { return typeof decl; }
    const saved = decl;
    assert.equal(saved(), 'function');
    decl = 42; // eslint-disable-line no-func-assign
    assert.equal(saved(), 'number');
    const ro = function named() { 'use strict'; named = 1; };
    assert.throws(ro, 'TypeError');
});

test('this, arguments and new.target in arrows', () => {
    function Ctor() {
        this.v = 7;
        const getThis = () => this.v;
        const getArgs = () => arguments.length;
        const getTarget = () => new.target === Ctor;
        this.r = [getThis(), getArgs(), getTarget()];
    }
    assert.deepEqual(new Ctor(1, 2).r, [7, 2, true]);
    const obj = {
        base() { return 'base'; },
    };
    const child = {
        __proto__: obj,
        m() { const up = () => super.base(); return up(); },
    };
    assert.equal(child.m(), 'base');
});

test('classes, private names and super', () => {
    class A {
        #secret = 11;
        static #count = 0;
        #priv() { return this.#secret + 1; }
        get value() { const read = () => this.#secret; return read(); }
        bump() { const inc = () => { A.#count++; return this.#priv(); }; return inc(); }
        static count() { return A.#count; }
    }
    class B extends A {
        constructor() {
            const init = () => super();
            init();
            this.extra = (() => this.value * 2)();
        }
    }
    const b = new B();
    assert.equal(b.value, 11);
    assert.equal(b.bump(), 12);
    assert.equal(A.count(), 1);
    assert.equal(b.extra, 22);
    function makeClass(x) {
        return class { get x() { return function () { return x; }(); } };
    }
    assert.equal(new (makeClass(9))().x, 9);
});

promise_test('async functions, generators and async arrows', async () => {
    let base = 100;
    async function af(n) { await null; return base + n; }
    const aa = async (n) => { await null; return base * n; };
    const ab = async n => base - n;
    function* gen(n) { for (let i = 0; i < n; i++) yield base + i; }
    async function* agen() { yield await af(1); yield 2; }
    assert.equal(await af(1), 101);
    assert.equal(await aa(2), 200);
    assert.equal(await ab(1), 99);
    assert.deepEqual([...gen(3)], [100, 101, 102]);
    const got = [];
    for await (const v of agen()) got.push(v);
    assert.deepEqual(got, [101, 2]);
    assert.equal(Object.getPrototypeOf(gen).constructor.name, 'GeneratorFunction');
    assert.equal(Object.getPrototypeOf(agen).constructor.name, 'AsyncGeneratorFunction');
});

test('length, name and text before the first call', () => {
    function three(a, b, c = 1, d) { return a; }
    const anon = function () {};
    const arr = (x, y) => x;
    assert.equal(three.length, 2);
    assert.equal(three.name, 'three');
    assert.equal(anon.name, 'anon');
    assert.equal(arr.length, 2);
    assert.equal(three.toString(), 'function three(a, b, c = 1, d) { return a; }');
    assert.equal(arr.toString(), '(x, y) => x');
    assert.equal(three(5), 5);
    assert.equal(three.toString(), 'function three(a, b, c = 1, d) { return a; }');
});

test('functions nested three deep', () => {
    const a = 1;
    function l1(b) {
        function l2(c) {
            function l3(d) {
                return () => a + b + c + d;
            }
            return l3;
        }
        return l2;
    }
    assert.equal(l1(2)(3)(4)(), 10);
    const inner = l1(10)(20);
    assert.equal(inner(30)(), 61);
});

test('default parameters and parameter scope', () => {
    const k = 'outer';
    function f(a = k, b = () => a, c = function () { return k + a; }) {
        const k = 'inner';
        return [a, b(), c(), k];
    }
    assert.deepEqual(f(), ['outer', 'outer', 'outerouter', 'inner']);
    assert.deepEqual(f('x'), ['x', 'x', 'outerx', 'inner']);
});

test('strict and sloppy functions', () => {
    function sloppy() { return this === undefined; }
    function strict() { 'use strict'; return this === undefined; }
    assert.equal(sloppy(), false);
    assert.equal(strict(), true);
    function sloppyOctal() { return 010; } // eslint-disable-line no-octal
    assert.equal(sloppyOctal(), 8);
});

test('with and eval keep their functions compiled with the script', () => {
    const scope = { w: 5 };
    let result;
    with (scope) { // eslint-disable-line no-with
        result = (function () { return function () { return w; }; })()();
    }
    assert.equal(result, 5);
    function viaEval() { var x = 3; return function () { return eval('x'); }; }
    assert.equal(viaEval()(), 3);
    function addsVar() { eval('var q = 4'); return function () { return q; }; }
    assert.equal(addsVar()(), 4);
    function evalInside() { const y = 8; return function () { return () => eval('y'); }; }
    assert.equal(evalInside()()(), 8);
});

test('block functions and annex B', () => {
    {
        function inBlock() { return 'block'; }
    }
    assert.equal(inBlock(), 'block');
    if (true) function single() { return 'single'; } // eslint-disable-line
    assert.equal(single(), 'single');
});

test('HTML comments in a script function', () => {
    function htmlComment() {
        var x = 1 <!-- this is a comment in a script, not in a module
        ;
        return x;
    }
    assert.equal(htmlComment(), 1);
});

test('templates, regular expressions and division around functions', () => {
    const n = 4;
    const t = `a${(function () { return n; })()}b${(() => `${n}`)()}`;
    assert.equal(t, 'a4b4');
    const re = function () { return /\/}\)/.test('/})'); };
    assert.equal(re(), true);
    const div = function (a) { return a / 2 / 1; };
    assert.equal(div(8), 4);
    const tag = (s, ...v) => s.raw.join('|') + v.join(',');
    assert.equal(tag`x${1}y${2}`, 'x|y|1,2');
});

test('a function compiled at its first call that calls itself deeply', () => {
    function depth(n) { return n === 0 ? 0 : 1 + depth(n - 1); }
    assert.equal(depth(500), 500);
});
