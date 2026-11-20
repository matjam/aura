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

test('named access on window: ids, and names of forms and images', () => {
    const box = document.createElement('div');
    box.innerHTML = '<p id="namedPara">p</p><form name="namedForm"></form><img name="twice"><img id="twice">' +
        '<span name="spanName"></span>';
    document.body.appendChild(box);
    try {
        assert.equal(window.namedPara, document.getElementById('namedPara'));
        assert.equal(namedPara.textContent, 'p', 'a bare name resolves');
        assert.equal(namedForm.localName, 'form');
        assert.ok('namedForm' in window);
        assert.equal(twice.length, 2, 'several matches make a collection');
        assert.equal(window.spanName, undefined, 'a span is not named by its name attribute');
        assert.throws(() => notAGlobalAnywhere, 'ReferenceError');
        window.namedPara = 5;
        assert.equal(window.namedPara, 5, 'a global the page sets wins');
        delete window.namedPara;
        assert.equal(window.location, location, "the window's own members win");
    } finally {
        box.remove();
    }
    assert.equal(window.namedForm, undefined, 'gone with the element');
});

promise_test('scheduler, permissions, locks, view transitions, Notification, BroadcastChannel', async () => {
    const order = [];
    const t = scheduler.postTask(() => { order.push('task'); return 7; });
    await scheduler.yield();
    order.push('yielded');
    assert.equal(await t, 7);
    assert.equal(order.join(), 'task,yielded');
    const ac = new AbortController();
    const aborted = scheduler.postTask(() => 1, { signal: ac.signal, delay: 50 });
    ac.abort();
    let reason = null;
    try { await aborted; } catch (e) { reason = e.name; }
    assert.equal(reason, 'AbortError');

    assert.equal((await navigator.permissions.query({ name: 'notifications' })).state, 'denied');

    const held = [];
    const a = navigator.locks.request('L', async (lock) => { held.push('a:' + lock.name); await delay(20); held.push('a done'); });
    const b = navigator.locks.request('L', (lock) => { held.push('b'); });
    const c = navigator.locks.request('L', { ifAvailable: true }, (lock) => { held.push('c:' + lock); });
    await Promise.all([a, b, c]);
    assert.equal(held.join(), 'a:L,c:null,a done,b', 'exclusive, in order; ifAvailable gets null while held');

    let ran = false;
    const vt = document.startViewTransition(() => { ran = true; });
    await vt.updateCallbackDone;
    await vt.finished;
    assert.ok(ran, 'the update runs');
    let skipped = null;
    try { await vt.ready; } catch (e) { skipped = e.name; }
    assert.equal(skipped, 'AbortError');

    assert.equal(Notification.permission, 'denied');
    assert.equal(await Notification.requestPermission(), 'denied');

    const ch1 = new BroadcastChannel('bc'), ch2 = new BroadcastChannel('bc');
    const got = new Promise((r) => { ch2.onmessage = (e) => r(e.data); });
    let self = 0;
    ch1.onmessage = () => self++;
    ch1.postMessage({ x: 1 });
    assert.equal((await got).x, 1);
    assert.equal(self, 0, 'a channel does not hear itself');
    ch1.close();
    ch2.close();

    /* these tests run without layout: the cases with a box are in
     * pages/img_events.html */
    assert.equal(document.createElement('div').checkVisibility(), false, 'not in the document');
    assert.equal(new DragEvent('dragstart').dataTransfer, null);
    assert.ok(new ClipboardEvent('copy') instanceof Event);
});

test('navigator and screen expose fixed values only', () => {
    assert.ok(navigator.userAgent.includes('Aura'));
    /* the platform the build runs on (core/uaplatform.h), as the user agent names it */
    const plat = { MacPPC: 'PPC Mac OS 9', MacIntel: 'Intel Mac OS X 10_15_7', 'Linux x86_64': 'Linux x86_64',
        'Linux aarch64': 'Linux aarch64' }[navigator.platform];
    assert.ok(plat && navigator.userAgent.includes(plat), navigator.platform + ' / ' + navigator.userAgent);
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

promise_test('MessageChannel delivers cloned messages in order, as tasks', async () => {
    const { port1, port2 } = new MessageChannel();
    assert.ok(port1 instanceof MessagePort && port1 instanceof EventTarget);
    const got = [];
    const obj = { n: 1 };
    port1.postMessage(obj);
    obj.n = 2;
    port1.postMessage('second');
    assert.equal(got.length, 0);
    port2.onmessage = (e) => got.push(e.data);
    assert.equal(got.length, 0, 'not delivered inside the call');
    await delay(10);
    assert.equal(got.length, 2);
    assert.equal(got[0].n, 1, 'cloned when posted');
    assert.equal(got[1], 'second');
    /* addEventListener needs start() */
    const back = [];
    port1.addEventListener('message', (e) => back.push(e.data));
    port2.postMessage('x');
    await delay(10);
    assert.equal(back.length, 0);
    port1.start();
    await delay(10);
    assert.equal(back.join(), 'x');
    port1.close();
    port2.postMessage('lost');
    await delay(10);
    assert.equal(back.length, 1);
    assert.throws(() => new MessagePort(), TypeError);
    /* an ArrayBuffer in the transfer list is emptied; anything else cannot be transferred */
    const buf = new ArrayBuffer(8);
    port2.postMessage(buf, [buf]);
    assert.equal(buf.byteLength, 0, 'the transferred buffer is detached');
    assert.throws(() => port2.postMessage(1, [{}]), 'DataCloneError');
    const keep = new Uint8Array([1, 2, 3]);
    const copy = structuredClone(keep, { transfer: [keep.buffer] });
    assert.equal(keep.byteLength, 0);
    assert.equal(copy[2], 3);
});

test('crypto gives random values where the embedder has a source', () => {
    const a = crypto.getRandomValues(new Uint8Array(64));
    assert.ok(a.some((x) => x !== 0), 'bytes were filled');
    const b = crypto.getRandomValues(new Uint32Array(4));
    assert.equal(b.length, 4);
    assert.throws(() => crypto.getRandomValues(new Float64Array(2)), 'TypeMismatchError');
    assert.throws(() => crypto.getRandomValues(new Uint8Array(65537)), 'QuotaExceededError');
    const u = crypto.randomUUID();
    assert.ok(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(u), u);
    assert.notEqual(u, crypto.randomUUID());
    assert.ok(crypto.subtle instanceof SubtleCrypto);
});

test('ARIA reflection, sandbox tokens and createCDATASection', () => {
    const d = document.createElement('div');
    assert.equal(d.ariaLabel, null);
    d.ariaLabel = 'Close';
    assert.equal(d.getAttribute('aria-label'), 'Close');
    d.role = 'button';
    assert.equal(d.getAttribute('role'), 'button');
    d.ariaLabel = null;
    assert.ok(!d.hasAttribute('aria-label'));
    d.setAttribute('aria-valuenow', '3');
    assert.equal(d.ariaValueNow, '3');
    assert.equal(document.createElement('iframe').sandbox.supports('allow-scripts'), false);
    assert.throws(() => document.createCDATASection('x'), 'NotSupportedError');
});

promise_test('postMessage takes a URL as the target origin', async () => {
    const got = [];
    const l = (e) => { if (e.data && e.data.tag === 'origin-test') got.push(e.data.n); };
    addEventListener('message', l);
    postMessage({ tag: 'origin-test', n: 1 }, location.href);
    postMessage({ tag: 'origin-test', n: 2 }, 'https://elsewhere.example/');
    postMessage({ tag: 'origin-test', n: 3 }, { targetOrigin: '*' });
    await delay(20);
    removeEventListener('message', l);
    assert.equal(got.join(), '1,3');
    assert.throws(() => postMessage(1, 'no url'), 'SyntaxError');
});

/* What a page that inspects its browser finds (hCaptcha's proof script
 * does all of this and reports the answers to its server). */
test('the window looks like a window to a script that inspects it', () => {
    assert.equal(Object.prototype.toString.call(window), '[object Window]');
    addEventListener('aura-inspect', () => {});
    document.addEventListener('aura-inspect', () => {});
    for (const o of [window, document, document.body]) {
        assert.equal(Reflect.ownKeys(o).filter((k) => typeof k === 'symbol').length, 0, 'no symbol keys');
        assert.equal(Object.getOwnPropertySymbols(o).length, 0);
        assert.equal(Object.getOwnPropertySymbols(Object.getOwnPropertyDescriptors(o)).length, 0);
    }
    const keys = Object.keys(window);
    for (const k of ['window', 'self', 'document', 'location', 'navigator', 'history', 'screen', 'performance',
        'localStorage', 'alert', 'fetch', 'setTimeout', 'getComputedStyle', 'getSelection', 'onclick', 'onload'])
        assert.ok(keys.includes(k), k + ' is an enumerable own property');
    assert.ok(!keys.includes('HTMLElement') && !keys.includes('Object'), 'interfaces are not enumerable');
});

test('a missing global is looked up without calling the page\'s document.getElementById', () => {
    const real = Document.prototype.getElementById, own = Object.getOwnPropertyDescriptor(document, 'getElementById');
    let calls = 0;
    const spy = function (id) { calls++; return real.call(this, id); };
    Document.prototype.getElementById = spy;
    document.getElementById = spy;
    try {
        assert.equal(typeof window.noSuchGlobalAnywhere, 'undefined');
        assert.ok(!('noSuchGlobalAnywhere' in window));
        assert.equal(calls, 0);
    } finally {
        Document.prototype.getElementById = real;
        if (own) Object.defineProperty(document, 'getElementById', own); else delete document.getElementById;
    }
});

test('navigator.plugins and mimeTypes are empty lists of their own kinds', () => {
    assert.equal(Object.prototype.toString.call(navigator.plugins), '[object PluginArray]');
    assert.equal(Object.prototype.toString.call(navigator.mimeTypes), '[object MimeTypeArray]');
    assert.ok(navigator.plugins instanceof PluginArray && navigator.plugins === navigator.plugins);
    assert.equal(navigator.plugins.length, 0);
    assert.equal(navigator.plugins.item(0), null);
    assert.equal(navigator.mimeTypes.namedItem('application/pdf'), null);
    assert.equal([...navigator.plugins].length, 0);
});

test('performance has the document\'s navigation entry', () => {
    const list = performance.getEntriesByType('navigation');
    assert.equal(list.length, 1);
    const e = list[0];
    assert.ok(e instanceof PerformanceNavigationTiming && e instanceof PerformanceResourceTiming && e instanceof PerformanceEntry);
    assert.equal(e.entryType, 'navigation');
    assert.equal(e.type, 'navigate');
    assert.equal(e.name, location.href);
    assert.equal(e.startTime, 0);
    assert.equal(typeof e.domInteractive, 'number');
    assert.equal(typeof e.toJSON().loadEventEnd, 'number');
    assert.ok(performance.getEntries().includes(e));
    assert.ok(PerformanceObserver.supportedEntryTypes.includes('navigation'));
});

test('no touch input: no ontouchstart, and a move has no button', () => {
    assert.ok(!('ontouchstart' in window) && !('ontouchstart' in document) && !('ontouchend' in document.body));
    assert.equal(navigator.maxTouchPoints, 0);
    assert.equal(new MouseEvent('mousemove', { button: 0, buttons: 0 }).which, 0, 'a move with no button down');
    assert.equal(new MouseEvent('mousemove', { button: 0, buttons: 1 }).which, 1, 'a drag');
    assert.equal(new MouseEvent('mousedown', { button: 0, buttons: 1 }).which, 1);
    assert.equal(new MouseEvent('mouseup', { button: 2, buttons: 0 }).which, 3);
});

test('navigator.oscpu is there only as Firefox', () => {
    assert.equal('oscpu' in navigator, / Firefox\//.test(navigator.userAgent));
});
