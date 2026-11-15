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
