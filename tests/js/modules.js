// Modules: dynamic import, static imports, import.meta.url, import maps,
// module scripts inserted by script.

promise_test('dynamic import of a module graph', async () => {
    const m = await import('./resources/mod_a.mjs');
    assert.equal(m.a, 'a');
    assert.equal(m.fromB, 'b+c');
    assert.ok(m.meta.endsWith('/resources/mod_a.mjs'));
    const again = await import('./resources/mod_a.mjs');
    assert.equal(again, m, 'modules are instantiated once');
});

promise_test('dynamic import failures reject', async () => {
    let err = null;
    try { await import('./resources/missing.mjs'); } catch (e) { err = e; }
    assert.ok(err instanceof TypeError, String(err));
    err = null;
    try { await import('bare-specifier'); } catch (e) { err = e; }
    assert.ok(err instanceof TypeError, String(err));
    err = null;
    try { await import('./resources/mod_syntax.mjs'); } catch (e) { err = e; }
    assert.ok(err instanceof SyntaxError, String(err));
});

promise_test('import maps map bare specifiers', async () => {
    const s = document.createElement('script');
    s.type = 'importmap';
    s.textContent = JSON.stringify({ imports: { 'lib-c': './resources/mod_c.mjs' } });
    document.head.appendChild(s);
    const m = await import('lib-c');
    assert.equal(m.c, 'c');
});

promise_test('module scripts inserted by script', async () => {
    window.__modRan = 0;
    const s = document.createElement('script');
    s.type = 'module';
    s.src = 'resources/mod_side.mjs';
    const p = once(s, 'load');
    document.head.appendChild(s);
    await p;
    assert.equal(window.__modRan, 1);
    const inline = document.createElement('script');
    inline.type = 'module';
    inline.textContent = 'import { c } from "./resources/mod_c.mjs"; window.__inline = c + import.meta.url.slice(0, 5);';
    document.head.appendChild(inline);
    await delay(10);
    assert.equal(window.__inline, 'cfile:');
});

promise_test('classic scripts inserted by script run, innerHTML scripts do not', async () => {
    window.__classic = [];
    const s = document.createElement('script');
    s.textContent = '__classic.push("inline:" + (document.currentScript !== null))';
    document.body.appendChild(s);
    assert.deepEqual(window.__classic, ['inline:true'], 'inline scripts run at insertion');
    const ext = document.createElement('script');
    ext.src = 'resources/classic.js';
    const p = once(ext, 'load');
    document.body.appendChild(ext);
    await p;
    assert.deepEqual(window.__classic, ['inline:true', 'external']);
    const d = document.createElement('div');
    d.innerHTML = '<script>__classic.push("never")<\/script>';
    document.body.appendChild(d);
    await delay(5);
    assert.equal(window.__classic.length, 2);
    const bad = document.createElement('script');
    bad.src = 'resources/nope.js';
    const pe = once(bad, 'error');
    document.body.appendChild(bad);
    await pe;
    const ignored = document.createElement('script');
    ignored.type = 'text/plain';
    ignored.textContent = '__classic.push("plain")';
    document.body.appendChild(ignored);
    assert.equal(window.__classic.length, 2);
});

promise_test('scripts inserted by script with async false run in insertion order', async () => {
    const made = document.createElement('script');
    assert.equal(made.async, true, 'a script made by script is async');
    made.async = false;
    assert.equal(made.async, false);
    assert.equal(made.hasAttribute('async'), false);
    made.async = true;
    assert.equal(made.hasAttribute('async'), true);
    // The file loads after the data: URL; with async false it still runs
    // first, as Google's module loader expects.
    window.__classic = [];
    const a = document.createElement('script');
    a.src = 'resources/classic.js';
    a.async = false;
    const b = document.createElement('script');
    b.src = 'data:text/javascript,__classic.push("second")';
    b.async = false;
    const pa = once(a, 'load'), pb = once(b, 'load');
    document.head.appendChild(a);
    document.head.appendChild(b);
    await pa;
    await pb;
    assert.deepEqual(window.__classic, ['external', 'second']);
    // A failed script does not hold back those after it.
    window.__classic = [];
    const bad = document.createElement('script');
    bad.src = 'resources/nope.js';
    bad.async = false;
    const c = document.createElement('script');
    c.src = 'data:text/javascript,__classic.push("after")';
    c.async = false;
    const pe = once(bad, 'error'), pc = once(c, 'load');
    document.head.appendChild(bad);
    document.head.appendChild(c);
    await pe;
    await pc;
    assert.deepEqual(window.__classic, ['after']);
});

// A module graph is fetched one level of imports at a time, and the
// modules compiled in an earlier round are resolved again, not compiled
// again (QuickJS patch 0010). Every module must still run once, in order,
// with its own import.meta.url.
promise_test('module graph with shared imports, loaded by two script elements', async () => {
    window.__graph = [];
    const a = document.createElement('script');
    a.type = 'module';
    a.src = 'resources/mod_graph_root.mjs';
    const b = document.createElement('script');
    b.type = 'module';
    b.src = 'resources/mod_graph_root.mjs';
    const pa = once(a, 'load'), pb = once(b, 'load');
    document.head.appendChild(a);
    document.head.appendChild(b);
    await pa;
    await pb;
    assert.deepEqual(window.__graph, ['leaf', 'shared', 'd1', 'd2', 'd3', 'd4', 'root'],
                     'each module runs once, imports first');
    assert.equal(window.__graphResult, '1sl,2sl,3sl3,4sl4');
    assert.ok(window.__graphMeta.endsWith('/resources/mod_graph_root.mjs'), window.__graphMeta);
    const metas = window.__graphMetas;
    assert.ok(metas[0].endsWith('/resources/mod_graph_d1.mjs'), metas[0]);
    assert.ok(metas[1].endsWith('/resources/mod_graph_shared.mjs'), metas[1]);
    assert.ok(metas[2].endsWith('/resources/mod_graph_leaf.mjs'), metas[2]);
    const m = await import('./resources/mod_graph_d4.mjs');
    assert.equal(m.d4, '4sl4');
    assert.equal(window.__graph.length, 7, 'importing a loaded module runs nothing');
});

promise_test('a graph with a syntax error fails every time', async () => {
    window.__graph = [];
    for (let i = 0; i < 2; i++) {
        let err = null;
        try { await import('./resources/mod_graph_bad.mjs'); } catch (e) { err = e; }
        assert.ok(err instanceof SyntaxError, String(err));
    }
    assert.equal(window.__badRan, undefined);
});

promise_test('an inline module whose imports arrive in two rounds', async () => {
    window.__graph = [];
    window.__inline2 = null;
    const s = document.createElement('script');
    s.type = 'module';
    s.textContent = 'import { shared } from "./resources/mod_graph_shared.mjs?inline";' +
        'window.__inline2 = shared + ":" + import.meta.url.slice(0, 5);';
    document.head.appendChild(s);
    for (let i = 0; i < 50 && window.__inline2 === null; i++) await delay(5);
    assert.equal(window.__inline2, 'sl:file:');
});
