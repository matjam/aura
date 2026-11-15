// Dedicated workers: messages, structured clone, transfers, ports, the
// worker global, importScripts, blob: and data: workers, errors, limits.

const next = (target, type = 'message') => new Promise((resolve) => target.addEventListener(type, resolve, { once: true }));
const ask = async (w, msg, transfer) => { const p = next(w); w.postMessage(msg, transfer); return (await p).data; };
const echoUrl = 'resources/worker_echo.js';

test('Worker exists; SharedWorker and ServiceWorker do not', () => {
    assert.equal(typeof Worker, 'function');
    assert.equal(typeof SharedWorker, 'undefined');
    assert.equal(typeof ServiceWorker, 'undefined');
    assert.equal('serviceWorker' in navigator, false);
    assert.throws(() => new Worker(), TypeError);
    assert.throws(() => new Worker('http://other.example/w.js'), 'SecurityError');
    assert.throws(() => new Worker(echoUrl, { type: 'bogus' }), TypeError);
});

promise_test('messages are structured clones, in order', async () => {
    const w = new Worker(echoUrl);
    const obj = { n: 1, list: [1, 'two', null, true, 1.5] };
    const p1 = ask(w, { cmd: 'echo', value: obj });
    obj.n = 2;
    const r1 = await p1;
    assert.equal(r1.echo.n, 1, 'cloned when posted');
    assert.deepEqual(r1.echo.list, [1, 'two', null, true, 1.5]);
    const r2 = await ask(w, { cmd: 'echo', value: 'x' });
    assert.equal(r2.count, 2);
    const shared = { s: 1 };
    const t = await ask(w, { cmd: 'types', value: { date: new Date(86400000), map: new Map([['k', 7]]), set: new Set([3]), re: /ab+c/gi,
        bytes: new Uint8Array([1, 2, 3]), a: shared, b: shared, blob: new Blob(['x'], { type: 'text/x-test' }), err: new RangeError('r'),
        big: 10n, u: undefined } });
    assert.deepEqual(t, { date: 86400000, map: 7, set: true, re: 'ab+c', bytes: [1, 2, 3], same: true, blob: 'text/x-test', err: 'r',
        big: 'bigint', undef: true });
    assert.throws(() => w.postMessage({ f() {} }), 'DataCloneError');
    assert.throws(() => w.postMessage(document.body), 'DataCloneError');
    w.terminate();
});

promise_test('the worker global has no DOM and the worker APIs', async () => {
    const w = new Worker(echoUrl, { name: 'bee' });
    const e = await ask(w, { cmd: 'env' });
    assert.equal(e.doc, 'undefined');
    assert.equal(e.win, 'undefined');
    assert.equal(e.raf, 'undefined');
    assert.ok(e.self && e.scope);
    assert.ok(e.href.endsWith('/resources/worker_echo.js'));
    assert.equal(e.ua, navigator.userAgent);
    assert.equal(e.name, 'bee');
    assert.equal(e.lib, 'lib-ok', 'importScripts ran the library');
    assert.equal(e.worker + e.shared + e.sw, 'undefinedundefinedundefined');
    assert.equal(e.api, 'function,function,function,function,function,function,function,number,function,function,function');
    w.terminate();
});

promise_test('an ArrayBuffer is transferred both ways', async () => {
    const w = new Worker(echoUrl);
    const buf = new Uint8Array([1, 2, 3, 4]).buffer;
    const p = next(w);
    w.postMessage({ cmd: 'transfer', buffer: buf }, [buf]);
    assert.equal(buf.byteLength, 0, 'detached in the sender');
    const r = (await p).data;
    assert.equal(r.len, 4);
    assert.deepEqual(Array.from(new Uint8Array(r.out)), [2, 4, 6, 8]);
    assert.equal((await next(w)).data.after, 0, 'detached in the worker');
    w.terminate();
});

promise_test('a MessagePort goes to a worker and one comes back', async () => {
    const w = new Worker(echoUrl);
    const ch = new MessageChannel();
    w.postMessage({ cmd: 'port' }, [ch.port2]);
    const p = next(ch.port1);
    ch.port1.start();
    ch.port1.postMessage('hi');
    assert.equal((await p).data, 'port:hi');
    const m = await next((w.postMessage({ cmd: 'makeport' }), w));
    assert.equal(m.ports.length, 1);
    const port = m.ports[0];
    assert.ok(port instanceof MessagePort);
    const q = next(port);
    port.start();
    port.postMessage([1, 2]);
    assert.deepEqual((await q).data.back, [1, 2]);
    w.terminate();
});

promise_test('timers, fetch, synchronous XMLHttpRequest and crypto.subtle in a worker', async () => {
    const w = new Worker(echoUrl);
    assert.equal((await ask(w, { cmd: 'timer' })).timer, true);
    const data = new URL('resources/data.json', document.URL).href;
    const f = await ask(w, { cmd: 'fetch', url: data });
    assert.equal(f.status, 200);
    assert.equal(f.name, 'aura');
    const x = await ask(w, { cmd: 'xhr', url: data });
    assert.equal(x.status, 200);
    assert.ok(x.text);
    const d = await ask(w, { cmd: 'digest', text: 'abc' });
    assert.equal(d.hex, 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
    w.terminate();
});

promise_test('an uncaught error in a worker reaches the Worker object', async () => {
    const w = new Worker(echoUrl);
    const p = next(w, 'error');
    w.postMessage({ cmd: 'throw' });
    const ev = await p;
    assert.ok(ev instanceof ErrorEvent);
    assert.ok(ev.message.includes('worker failure'));
    assert.ok(ev.filename.endsWith('worker_echo.js'));
    assert.ok(ev.lineno > 0);
    w.terminate();
});

promise_test('a script that cannot be loaded fires error', async () => {
    const w = new Worker('resources/no_such_worker.js');
    const ev = await next(w, 'error');
    assert.equal(ev.type, 'error');
});

promise_test('close() ends the worker after the current task', async () => {
    const w = new Worker(echoUrl);
    const got = [];
    w.onmessage = (e) => got.push(Object.keys(e.data)[0]);
    w.postMessage({ cmd: 'close' });
    w.postMessage({ cmd: 'echo', value: 1 });
    await delay(50);
    assert.deepEqual(got, ['closing', 'afterClose'], 'the task finished, later messages were dropped');
});

promise_test('blob: and data: URL workers', async () => {
    const src = 'onmessage = (e) => postMessage({ twice: e.data * 2, origin: self.origin, href: location.href.slice(0, 5) });';
    const url = URL.createObjectURL(new Blob([src], { type: 'text/javascript' }));
    const w = new Worker(url);
    const r = await ask(w, 21);
    assert.equal(r.twice, 42);
    assert.equal(r.href, 'blob:');
    w.terminate();
    URL.revokeObjectURL(url);
    const d = new Worker('data:text/javascript,' + encodeURIComponent(src));
    const r2 = await ask(d, 4);
    assert.equal(r2.twice, 8);
    assert.equal(r2.origin, 'null', 'a data: worker has an opaque origin');
    d.terminate();
});

promise_test('a module worker', async () => {
    const w = new Worker('resources/worker_mod.mjs', { type: 'module' });
    const r = await ask(w, 'm');
    assert.ok(r.module && r.meta);
    assert.equal(r.got, 'm');
    w.terminate();
});

promise_test('a long computation in a worker does not stop the page', async () => {
    const w = new Worker(echoUrl);
    let ticks = 0;
    const iv = setInterval(() => ticks++, 5);
    const r = await ask(w, { cmd: 'spin', ms: 200 });
    clearInterval(iv);
    assert.ok(r.spun);
    assert.ok(ticks >= 5, 'page timers fired while the worker computed: ' + ticks);
    /* a worker in an endless loop is ended by terminate() */
    w.postMessage({ cmd: 'forever' });
    await delay(30);
    w.terminate();
    await delay(10);
});

promise_test('a page has at most eight workers', async () => {
    const ws = [];
    for (let i = 0; i < 8; i++) ws.push(new Worker(echoUrl));
    const ninth = new Worker(echoUrl);
    const ev = await next(ninth, 'error');
    assert.equal(ev.type, 'error');
    for (const w of ws) w.terminate();
    await delay(20);
    const again = new Worker(echoUrl);
    assert.equal((await ask(again, { cmd: 'echo', value: 5 })).echo, 5, 'a slot is free again after terminate()');
    again.terminate();
});
