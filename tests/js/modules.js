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
