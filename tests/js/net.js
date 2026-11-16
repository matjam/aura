// fetch, Headers, Request, Response, XMLHttpRequest, FormData over the
// fetch interface; same-origin enforcement and the CORS points.

const here = new URL('.', document.URL).href;

test('Headers', () => {
    const h = new Headers({ 'Content-Type': 'text/plain', 'X-A': '1' });
    h.append('x-a', '2');
    assert.equal(h.get('X-A'), '1, 2');
    assert.ok(h.has('content-type'));
    assert.deepEqual([...h.keys()], ['content-type', 'x-a']);
    h.delete('x-a');
    assert.equal(h.get('x-a'), null);
    assert.throws(() => h.append('bad name', 'v'), TypeError);
    assert.throws(() => h.append('x', 'a\nb'), TypeError);
});

test('Request and Response', () => {
    const r = new Request('data.json', { method: 'post', body: 'x', headers: { 'Cookie': 'no', 'X-Ok': '1' } });
    assert.equal(r.method, 'POST');
    assert.ok(r.url.endsWith('/data.json'));
    assert.equal(r.headers.get('cookie'), null, 'forbidden request headers are dropped');
    assert.equal(r.headers.get('x-ok'), '1');
    assert.throws(() => new Request('x', { method: 'GET', body: 'b' }), TypeError);
    assert.throws(() => new Request('x', { method: 'TRACE' }), TypeError);
    const res = new Response('{"a":1}', { status: 201, headers: { 'content-type': 'application/json' } });
    assert.equal(res.status, 201);
    assert.ok(res.ok);
    assert.throws(() => new Response('', { status: 99 }), RangeError);
    assert.equal(Response.error().type, 'error');
    assert.equal(Response.redirect('http://a/', 301).headers.get('location'), 'http://a/');
});

promise_test('Response body methods', async () => {
    const res = new Response('{"a":1}');
    assert.equal((await res.clone().json()).a, 1);
    assert.equal(await res.text(), '{"a":1}');
    await res.text().then(() => { throw new Error('a used body was read again'); }, () => {});
    const j = Response.json({ b: 2 });
    assert.equal(j.headers.get('content-type'), 'application/json');
    assert.equal((await new Response(new Uint8Array([104, 105])).text()), 'hi');
});

promise_test('fetch a same-origin file', async () => {
    const res = await fetch('resources/data.json');
    assert.equal(res.status, 200);
    assert.equal(res.type, 'basic');
    assert.equal(res.headers.get('content-type'), 'application/json');
    const j = await res.json();
    assert.equal(j.name, 'aura');
});

promise_test('fetch a missing file rejects with TypeError', async () => {
    let err = null;
    try { await fetch('resources/missing.json'); } catch (e) { err = e; }
    assert.ok(err instanceof TypeError);
});

promise_test('fetch refuses cross-origin requests in same-origin mode', async () => {
    let err = null;
    try { await fetch('http://example.com/', { mode: 'same-origin' }); } catch (e) { err = e; }
    assert.ok(err instanceof TypeError);
});

promise_test('CORS point: file: documents cannot read network origins', async () => {
    let err = null;
    try { await fetch('http://example.com/data'); } catch (e) { err = e; }
    assert.ok(err instanceof TypeError, 'cross-origin from file: is a network error');
});

promise_test('PREFLIGHT point: non-simple cross-origin requests are refused', async () => {
    let err = null;
    try { await fetch('http://example.com/', { method: 'PUT' }); } catch (e) { err = e; }
    assert.ok(err instanceof TypeError);
});

promise_test('unsupported schemes are refused', async () => {
    let err = null;
    try { await fetch('ftp://example.com/'); } catch (e) { err = e; }
    assert.ok(err instanceof TypeError);
});

promise_test('fetch data: and blob: URLs', async () => {
    assert.equal(await (await fetch('data:text/plain,hello%20there')).text(), 'hello there');
    assert.equal(await (await fetch('data:;base64,aGk=')).text(), 'hi');
    const u = URL.createObjectURL(new Blob(['blobbed'], { type: 'text/x' }));
    const r = await fetch(u);
    assert.equal(r.headers.get('content-type'), 'text/x');
    assert.equal(await r.text(), 'blobbed');
    URL.revokeObjectURL(u);
    let err = null;
    try { await fetch(u); } catch (e) { err = e; }
    assert.ok(err instanceof TypeError);
});

promise_test('fetch abort', async () => {
    const ac = new AbortController();
    const p = fetch('resources/data.json', { signal: ac.signal });
    ac.abort();
    let err = null;
    try { await p; } catch (e) { err = e; }
    assert.equal(err && err.name, 'AbortError');
});

promise_test('XMLHttpRequest', async () => {
    const x = new XMLHttpRequest();
    const states = [];
    x.onreadystatechange = () => states.push(x.readyState);
    x.open('GET', 'resources/data.json');
    x.responseType = 'json';
    x.send();
    await once(x, 'loadend');
    assert.equal(x.status, 200);
    assert.equal(x.response.name, 'aura');
    assert.equal(states[0], 1);
    assert.equal(states[states.length - 1], 4);
    assert.ok(x.getAllResponseHeaders().includes('content-type: application/json'));
    // a synchronous request of a document opens, and fails when sent (no network inside a script call)
    assert.throws(() => x.open('GET', 'resources/data.json', false), 'InvalidAccessError', 'with a responseType');
    const sy = new XMLHttpRequest();
    sy.open('GET', 'resources/data.json', false);
    assert.throws(() => sy.send(), 'NetworkError');
    const d = new XMLHttpRequest();
    d.open('GET', 'data:text/plain,sync%20ok', false);
    d.send();
    assert.equal(d.status, 200);
    assert.equal(d.responseText, 'sync ok');
});

promise_test('XMLHttpRequest errors and abort', async () => {
    const x = new XMLHttpRequest();
    x.open('GET', 'resources/missing.txt');
    x.send();
    await once(x, 'error');
    assert.equal(x.status, 0);
    const y = new XMLHttpRequest();
    y.open('GET', 'resources/data.json');
    y.send();
    let aborted = false;
    y.onabort = () => { aborted = true; };
    y.abort();
    assert.ok(aborted);
    assert.equal(y.readyState, 0);
    const z = new XMLHttpRequest();
    z.open('GET', 'http://example.com/');
    z.send();
    await once(z, 'error');
});

test('FormData', () => {
    const fd = new FormData();
    fd.append('a', '1');
    fd.append('f', new Blob(['x']), 'x.txt');
    fd.set('a', '2');
    assert.equal(fd.get('a'), '2');
    assert.equal(fd.get('f').name, 'x.txt');
    assert.deepEqual([...fd.keys()], ['a', 'f']);
    assert.throws(() => new FormData(document.body), TypeError);
});

test('Request carries the fetch options', () => {
    const r = new Request('x', { mode: 'no-cors', credentials: 'include', cache: 'no-store', redirect: 'manual',
        referrerPolicy: 'no-referrer', integrity: 'sha256-abc', keepalive: true, referrer: '' });
    assert.deepEqual([r.mode, r.credentials, r.cache, r.redirect, r.referrerPolicy, r.integrity, r.keepalive, r.referrer],
        ['no-cors', 'include', 'no-store', 'manual', 'no-referrer', 'sha256-abc', true, '']);
    const d = new Request('x');
    assert.deepEqual([d.mode, d.credentials, d.cache, d.redirect, d.referrer, d.destination], ['cors', 'same-origin', 'default', 'follow', 'about:client', '']);
    const c = new Request(r, { cache: 'reload' });
    assert.equal(c.cache, 'reload');
    assert.equal(c.redirect, 'manual', 'the rest is copied');
    assert.ok(d.signal instanceof AbortSignal);
    assert.throws(() => new Request('x', { mode: 'navigate' }), TypeError);
    assert.throws(() => new Request('x', { cache: 'only-if-cached' }), TypeError);
    assert.throws(() => new Request('x', { mode: 'no-cors', method: 'PUT' }), TypeError);
    assert.throws(() => new Request('x', { credentials: 'sometimes' }), TypeError);
    assert.throws(() => new Request('http://user:pw@example.com/'), TypeError);
    /* a no-cors request keeps only safelisted headers */
    const n = new Request('x', { mode: 'no-cors', headers: { 'X-A': '1', Accept: 'text/plain', 'Content-Type': 'application/json' } });
    assert.deepEqual([...n.headers.keys()], ['accept']);
    /* forbidden headers, and the method override ones that name a forbidden method */
    const h = new Request('x', { headers: { 'Sec-Fetch-Mode': 'x', 'Proxy-A': 'x', 'X-HTTP-Method-Override': 'TRACE', 'X-Ok': '1' } }).headers;
    assert.deepEqual([...h.keys()], ['x-ok']);
    assert.equal(new Request('x', { method: 'post', body: new URLSearchParams('a=1') }).headers.get('content-type'),
        'application/x-www-form-urlencoded;charset=UTF-8');
    assert.equal(new Request('x', { method: 'POST', body: new Blob(['b'], { type: 'text/x-b' }) }).headers.get('content-type'), 'text/x-b');
    assert.equal(new Request('x', { method: 'POST', body: new Uint8Array(2) }).headers.get('content-type'), null);
    assert.equal(new Response('x').body, undefined, 'there are no streams');
});

promise_test('Response bodies: bytes, formData, clone', async () => {
    const fd = new FormData();
    fd.append('name', 'value with spaces');
    fd.append('file', new File(['\x00\x01binary\r\n'], 'a b.bin', { type: 'application/x-test' }));
    const req = new Request('x', { method: 'POST', body: fd });
    const back = await new Response(await req.arrayBuffer(), { headers: { 'content-type': req.headers.get('content-type') } }).formData();
    assert.equal(back.get('name'), 'value with spaces');
    const f = back.get('file');
    assert.ok(f instanceof File);
    assert.equal(f.name, 'a b.bin');
    assert.equal(f.type, 'application/x-test');
    assert.deepEqual(Array.from(await f.bytes()), [0, 1, 98, 105, 110, 97, 114, 121, 13, 10]);
    const u = await new Response('a=1&b=two', { headers: { 'content-type': 'application/x-www-form-urlencoded' } }).formData();
    assert.equal(u.get('b'), 'two');
    const r = new Response('abc');
    const c = r.clone();
    assert.deepEqual(Array.from(await r.bytes()), [97, 98, 99]);
    assert.equal(await c.text(), 'abc');
    assert.equal((await new Response('x', { headers: { 'Content-Type': 'text/x-q' } }).blob()).type, 'text/x-q');
});

promise_test('fetch checks integrity metadata', async () => {
    /* sha256 of resources/data.json, computed here */
    const bytes = await (await fetch('resources/data.json')).bytes();
    const b64 = btoa(String.fromCharCode(...new Uint8Array(await crypto.subtle.digest('SHA-256', bytes))));
    const b512 = btoa(String.fromCharCode(...new Uint8Array(await crypto.subtle.digest('SHA-512', bytes))));
    assert.equal((await fetch('resources/data.json', { integrity: 'sha256-' + b64 })).status, 200);
    assert.equal((await fetch('resources/data.json', { integrity: 'sha256-AAAA sha512-' + b512 })).status, 200, 'the strongest algorithm decides');
    assert.equal((await fetch('resources/data.json', { integrity: 'md5-zzzz' })).status, 200, 'unknown algorithms are ignored');
    let err = null;
    try { await fetch('resources/data.json', { integrity: 'sha256-' + b64.replace(/^./, (c) => (c === 'A' ? 'B' : 'A')) }); } catch (e) { err = e; }
    assert.ok(err instanceof TypeError, 'a wrong digest is a network error');
});

promise_test('crypto.subtle: digests and HMAC', async () => {
    const hex = (b) => Array.from(new Uint8Array(b), (x) => x.toString(16).padStart(2, '0')).join('');
    const abc = new TextEncoder().encode('abc');
    assert.equal(hex(await crypto.subtle.digest('SHA-1', abc)), 'a9993e364706816aba3e25717850c26c9cd0d89d');
    assert.equal(hex(await crypto.subtle.digest({ name: 'sha-256' }, abc.buffer)), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
    assert.equal(hex(await crypto.subtle.digest('SHA-384', abc)).slice(0, 16), 'cb00753f45a35e8b');
    assert.equal(hex(await crypto.subtle.digest('SHA-512', abc)).slice(0, 16), 'ddaf35a193617aba');
    let err = null;
    try { await crypto.subtle.digest('MD5', abc); } catch (e) { err = e; }
    assert.equal(err && err.name, 'NotSupportedError');
    const key = await crypto.subtle.importKey('raw', new TextEncoder().encode('Jefe'), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign', 'verify']);
    assert.ok(key instanceof CryptoKey);
    const msg = new TextEncoder().encode('what do ya want for nothing?');
    const sig = await crypto.subtle.sign('HMAC', key, msg);
    assert.equal(hex(sig), '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843');
    assert.equal(await crypto.subtle.verify('HMAC', key, sig, msg), true);
    assert.equal(await crypto.subtle.verify('HMAC', key, sig, abc), false);
    err = null;
    try { await crypto.subtle.exportKey('raw', key); } catch (e) { err = e; }
    assert.equal(err && err.name, 'InvalidAccessError');
    err = null;
    try { await crypto.subtle.encrypt({ name: 'AES-GCM', iv: new Uint8Array(12) }, key, msg); } catch (e) { err = e; }
    assert.equal(err && err.name, 'InvalidAccessError', 'an HMAC key does not encrypt');
    err = null;
    try { await crypto.subtle.encrypt({ name: 'AES-GCM' }, key, msg); } catch (e) { err = e; }
    assert.ok(err instanceof TypeError, 'AES-GCM without an iv');
});

promise_test('XMLHttpRequest: event order, response types, headers', async () => {
    const x = new XMLHttpRequest();
    const log = [];
    for (const t of ['loadstart', 'progress', 'load', 'loadend', 'error', 'abort', 'timeout']) x.addEventListener(t, (e) => log.push(t + ':' + e.loaded));
    x.onreadystatechange = () => log.push('rs' + x.readyState);
    x.open('GET', 'resources/data.json');
    x.overrideMimeType('text/plain; charset=utf-8');
    x.responseType = 'arraybuffer';
    x.send();
    await once(x, 'loadend');
    const n = x.response.byteLength;
    assert.ok(x.response instanceof ArrayBuffer && n > 0);
    assert.deepEqual(log, ['rs1', 'loadstart:0', 'rs2', 'rs3', 'progress:' + n, 'progress:' + n, 'rs4', 'load:' + n, 'loadend:' + n]);
    assert.throws(() => x.responseXML, 'InvalidStateError');
    assert.ok(x.responseURL.endsWith('resources/data.json'));
    assert.equal(x.getResponseHeader('Content-Type'), 'application/json');
    assert.equal(x.getResponseHeader('X-None'), null);
    assert.throws(() => { x.responseType = 'text'; }, 'InvalidStateError');
    assert.throws(() => x.overrideMimeType('text/plain'), 'InvalidStateError');
    const t = new XMLHttpRequest();
    t.open('GET', 'resources/data.json');
    t.responseType = 'blob';
    t.send();
    await once(t, 'load');
    assert.ok(t.response instanceof Blob);
    assert.equal(t.response.type, 'application/json');
    const d = new XMLHttpRequest();
    d.open('GET', 'data:text/html,<p id=a>doc</p>');
    d.responseType = 'document';
    d.send();
    await once(d, 'load');
    assert.equal(d.responseXML.getElementById('a').textContent, 'doc');
    assert.throws(() => new XMLHttpRequest().open('TRACE', 'x'), 'SecurityError');
    assert.throws(() => new XMLHttpRequest().setRequestHeader('a', 'b'), 'InvalidStateError');
    assert.equal(navigator.sendBeacon('http://example.com/b', 'x'), false, 'a file: document cannot reach the network');
});
