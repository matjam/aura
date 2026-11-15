// JavaScript benchmarks for Aura, the same on the host (`aura-host js
// --stats tests/js/bench/bench.js`) and on Mac OS 9 (JSTest, which embeds
// this file). Each line is "bench NAME MS". globalThis.BENCH_SCALE (default
// 1) multiplies the work; JSTest sets it lower for slow machines.
(function () {
    'use strict';
    const scale = typeof globalThis.BENCH_SCALE === 'number' ? globalThis.BENCH_SCALE : 1;
    const n = (k) => Math.max(1, Math.round(k * scale));
    let sink = 0;
    const results = [];
    function bench(name, fn) {
        const t0 = performance.now();
        fn();
        const ms = performance.now() - t0;
        results.push([name, ms]);
        console.log('bench ' + name + ' ' + ms.toFixed(1));
    }

    bench('arith-loop', () => {
        let s = 0;
        for (let i = 0, m = n(2000000); i < m; i++) s = (s + i * 3) | 0;
        sink += s;
    });
    bench('fib-calls', () => {
        const fib = (k) => (k < 2 ? k : fib(k - 1) + fib(k - 2));
        sink += fib(scale >= 1 ? 25 : 20);
    });
    bench('object-props', () => {
        class P { constructor(x, y) { this.x = x; this.y = y; } len() { return this.x * this.x + this.y * this.y; } }
        let s = 0;
        for (let i = 0, m = n(300000); i < m; i++) s += new P(i, i + 1).len() & 255;
        sink += s;
    });
    bench('string-build', () => {
        const parts = [];
        for (let i = 0, m = n(40000); i < m; i++) parts.push('item' + i);
        let s = parts.join(',');
        s = s.replace(/item(\d)/g, 'x$1').toUpperCase();
        sink += s.length;
    });
    bench('array-sort', () => {
        const a = [];
        let x = 12345;
        for (let i = 0, m = n(50000); i < m; i++) { x = (x * 1103515245 + 12345) & 0x7fffffff; a.push(x); }
        a.sort((p, q) => p - q);
        sink += a[0];
    });
    bench('regexp', () => {
        const re = /([a-z]+)@([a-z]+)\.(com|org)/;
        let c = 0;
        for (let i = 0, m = n(50000); i < m; i++) if (re.test('user' + i + ' mail' + (i % 7) + '@example.' + (i & 1 ? 'com' : 'net'))) c++;
        sink += c;
    });
    bench('json', () => {
        const o = [];
        for (let i = 0, m = n(5000); i < m; i++) o.push({ id: i, name: 'n' + i, tags: ['a', 'b'], v: i / 3 });
        const s = JSON.stringify(o);
        sink += JSON.parse(s).length + s.length;
    });
    bench('dom-create', () => {
        const root = document.createElement('div');
        for (let i = 0, m = n(5000); i < m; i++) {
            const e = document.createElement('p');
            e.className = 'c' + (i % 10);
            e.setAttribute('data-i', String(i));
            e.appendChild(document.createTextNode('text ' + i));
            root.appendChild(e);
        }
        document.body.appendChild(root);
        sink += root.childNodes.length;
        globalThis.__benchRoot = root;
    });
    bench('dom-query', () => {
        let c = 0;
        for (let i = 0, m = n(200); i < m; i++) c += document.querySelectorAll('p.c3').length;
        for (let i = 0, m = n(2000); i < m; i++) c += document.getElementsByTagName('p').length;
        sink += c;
    });
    bench('dom-innerhtml', () => {
        const d = document.createElement('div');
        let html = '';
        for (let i = 0; i < 200; i++) html += '<li class="x"><a href="#' + i + '">link ' + i + '</a> <b>bold</b></li>';
        for (let i = 0, m = n(50); i < m; i++) { d.innerHTML = '<ul>' + html + '</ul>'; sink += d.innerHTML.length; }
    });
    bench('dom-events', () => {
        const t = globalThis.__benchRoot.firstChild;
        let c = 0;
        globalThis.__benchRoot.addEventListener('ping', () => c++);
        t.addEventListener('ping', () => c++);
        for (let i = 0, m = n(20000); i < m; i++) t.dispatchEvent(new Event('ping', { bubbles: true }));
        sink += c;
    });
    bench('dom-text', () => {
        let s = 0;
        for (let i = 0, m = n(50); i < m; i++) s += globalThis.__benchRoot.textContent.length;
        globalThis.__benchRoot.remove();
        sink += s;
    });
    let total = 0;
    for (const [, ms] of results) total += ms;
    console.log('bench total ' + total.toFixed(1));
    globalThis.__benchSink = sink;
})();
