// Window members: location, history, navigator, screen, dialogs,
// matchMedia, structuredClone, atob/btoa, URL, TextEncoder, storage.

test('location', () => {
    assert.equal(location.href, document.URL);
    assert.equal(location.protocol, 'file:');
    assert.ok(location.pathname.endsWith('.js'));
    assert.equal(String(location), location.href);
    assert.ok(location instanceof Location);
});

test('history.pushState changes the URL and state', () => {
    const before = location.href;
    history.pushState({ a: 1 }, '', '?page=2');
    assert.ok(location.href.endsWith('?page=2'));
    assert.equal(history.state.a, 1);
    history.replaceState(null, '', before);
    assert.equal(location.href, before);
    assert.throws(() => history.pushState(null, '', 'http://other.example/'), 'SecurityError');
    assert.throws(() => history.pushState({ f() {} }, ''), 'DataCloneError');
});

test('navigator and screen expose fixed values only', () => {
    assert.ok(navigator.userAgent.includes('Aura'));
    assert.equal(navigator.platform, 'MacPPC');
    assert.equal(navigator.hardwareConcurrency, 1);
    assert.equal(navigator.plugins.length, 0);
    assert.equal(navigator.languages[0], navigator.language);
    assert.equal(navigator.webdriver, false);
    assert.equal(typeof screen.width, 'number');
    assert.equal(screen.colorDepth, 24);
    assert.equal(navigator.deviceMemory, undefined);
    assert.equal(navigator.getBattery, undefined);
});

test('dialogs without an embedder', () => {
    assert.equal(alert('x'), undefined);
    assert.equal(confirm('x'), false);
    assert.equal(prompt('x'), null);
    assert.equal(open('about:blank'), null);
});

test('viewport and scrolling members', () => {
    assert.equal(typeof innerWidth, 'number');
    assert.equal(scrollX, 0);
    scrollTo(0, 100);
    scrollBy({ top: 5 });
    assert.equal(devicePixelRatio, 1);
});

test('matchMedia', () => {
    const m = matchMedia('(min-width: 1px)');
    assert.ok(m instanceof MediaQueryList);
    assert.equal(m.media, '(min-width: 1px)');
    assert.equal(typeof m.matches, 'boolean');
    m.addEventListener('change', () => {});
    m.addListener(() => {});
});

test('structuredClone', () => {
    const src = { a: [1, 2, { b: 'c' }], d: new Date(5), m: new Map([[1, 2]]), s: new Set([3]), r: /x/g, u: new Uint8Array([1, 2]) };
    src.self = src;
    const c = structuredClone(src);
    assert.notEqual(c, src);
    assert.equal(c.self, c);
    assert.equal(c.a[2].b, 'c');
    assert.equal(c.d.getTime(), 5);
    assert.equal(c.m.get(1), 2);
    assert.ok(c.s.has(3));
    assert.equal(c.r.flags, 'g');
    assert.equal(c.u[1], 2);
    assert.throws(() => structuredClone(() => 1), 'DataCloneError');
    assert.throws(() => structuredClone(document.body), 'DataCloneError');
    assert.equal(structuredClone(new RangeError('e')).name, 'RangeError');
});

test('atob and btoa', () => {
    assert.equal(btoa('hello'), 'aGVsbG8=');
    assert.equal(atob('aGVsbG8='), 'hello');
    assert.throws(() => btoa('Ā'), 'InvalidCharacterError');
    assert.throws(() => atob('!'), 'InvalidCharacterError');
});

test('URL parsing and setters', () => {
    const u = new URL('https://user:pw@example.com:8080/a/../b/c?x=1&y=2#frag');
    assert.equal(u.href, 'https://user:pw@example.com:8080/b/c?x=1&y=2#frag');
    assert.equal(u.origin, 'https://example.com:8080');
    assert.equal(u.host, 'example.com:8080');
    assert.equal(u.searchParams.get('y'), '2');
    u.searchParams.append('z', 'a b');
    assert.equal(u.search, '?x=1&y=2&z=a+b');
    u.port = '443';
    assert.equal(u.port, '');
    u.pathname = 'q r';
    assert.equal(u.pathname, '/q%20r');
    u.hash = '';
    assert.equal(u.href, 'https://user:pw@example.com/q%20r?x=1&y=2&z=a+b');
    assert.equal(new URL('../x', 'http://h/a/b/c').href, 'http://h/a/x');
    assert.throws(() => new URL('nope'), TypeError);
    assert.ok(URL.canParse('http://a'));
    assert.ok(!URL.canParse('::'));
    assert.equal(new URL('HTTP://EXAMPLE.com').href, 'http://example.com/');
    assert.equal(new URL('data:text/plain,hi').protocol, 'data:');
});

test('URL uses the WHATWG host parser', () => {
    assert.equal(new URL('http://0x7f.1/').hostname, '127.0.0.1');
    assert.equal(new URL('http://[::1]:81/').host, '[::1]:81');
    assert.equal(new URL('http://b\u00fccher.example/').hostname, 'xn--bcher-kva.example');
    assert.equal(new URL('http://a/%2e%2E/b').pathname, '/b');
    assert.equal(new URL('file:///C:/x/../y').pathname, '/C:/y');
    assert.throws(() => new URL('http://a b/'), TypeError);
    const a = document.createElement('a');
    a.href = 'https://Example.COM:443/p?q#h';
    assert.equal(a.host, 'example.com');
    assert.equal(a.pathname, '/p');
});

test('URLSearchParams', () => {
    const p = new URLSearchParams('?a=1&b=2&a=3');
    assert.deepEqual(p.getAll('a'), ['1', '3']);
    p.set('a', 'x');
    assert.equal(p.toString(), 'a=x&b=2');
    p.sort();
    p.delete('b');
    assert.equal(p.toString(), 'a=x');
    assert.equal(new URLSearchParams({ k: 'v w' }).toString(), 'k=v+w');
    assert.equal(new URLSearchParams([['q', '%']]).toString(), 'q=%25');
    assert.equal(new URLSearchParams('x=%zz').get('x'), '%zz');
    assert.equal(p.size, 1);
});

test('TextEncoder and TextDecoder', () => {
    const b = new TextEncoder().encode('hé\u{1f600}');
    assert.deepEqual(Array.from(b), [104, 195, 169, 240, 159, 152, 128]);
    assert.equal(new TextDecoder().decode(b), 'hé\u{1f600}');
    assert.equal(new TextDecoder().decode(new Uint8Array([0xff])), '�');
    assert.throws(() => new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array([0xff])), TypeError);
    assert.equal(new TextDecoder().decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x41])), 'A');
    const d = new TextDecoder();
    assert.equal(d.decode(new Uint8Array([0xe2, 0x82]), { stream: true }) + d.decode(new Uint8Array([0xac])), '€');
    assert.equal(new TextDecoder('latin1').decode(new Uint8Array([0x80, 0xe9])), '€é');
    assert.throws(() => new TextDecoder('nope'), RangeError);
    const out = new Uint8Array(4);
    const r = new TextEncoder().encodeInto('aé\u{1f600}', out);
    assert.deepEqual([r.read, r.written], [2, 3]);
});

test('localStorage and sessionStorage', () => {
    localStorage.clear();
    localStorage.setItem('a', 1);
    assert.equal(localStorage.getItem('a'), '1');
    localStorage.b = 'two';
    assert.equal(localStorage.length, 2);
    assert.deepEqual(Object.keys(localStorage).sort(), ['a', 'b']);
    delete localStorage.a;
    assert.equal(localStorage.getItem('a'), null);
    assert.equal(localStorage.key(0), 'b');
    sessionStorage.setItem('s', 'x');
    assert.equal(sessionStorage.s, 'x');
    assert.throws(() => localStorage.setItem('big', 'x'.repeat(3 * 1024 * 1024)), 'QuotaExceededError');
    assert.ok(localStorage instanceof Storage);
});

test('Blob, File, FileReader', async () => {
    const b = new Blob(['ab', new Uint8Array([99])], { type: 'Text/Plain' });
    assert.equal(b.size, 3);
    assert.equal(b.type, 'text/plain');
    assert.equal(b.slice(1).size, 2);
    const f = new File(['x'], 'n.txt');
    assert.equal(f.name, 'n.txt');
});

promise_test('Blob.text and FileReader', async () => {
    const b = new Blob(['hé']);
    assert.equal(await b.text(), 'hé');
    const r = new FileReader();
    r.readAsDataURL(new Blob(['hi'], { type: 'text/plain' }));
    await once(r, 'load');
    assert.equal(r.result, 'data:text/plain;base64,aGk=');
});

promise_test('postMessage to self', async () => {
    const p = once(window, 'message');
    postMessage({ v: 1 }, '*');
    const e = await p;
    assert.equal(e.data.v, 1);
});
