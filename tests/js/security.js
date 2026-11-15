// Hostile input: wrong receivers and arguments for every binding, internals
// out of reach, prototype tampering.

test('internals are not reachable', () => {
    assert.equal(typeof N, 'undefined');
    assert.equal(typeof P, 'undefined');
    // The prelude is loaded from bytecode without its source, so its
    // functions print as native code; a build that embeds the source
    // (AURA_JS_PRELUDE_SOURCE) prints the method as written.
    assert.ok(String(print) === 'print() {}' || /^function print\(\) \{\s+\[native code\]\s+\}$/.test(String(print)),
              'print is window.print, not the engine print: ' + String(print));
    assert.equal(print.length, 0);
    assert.equal(typeof std, 'undefined');
    assert.equal(typeof os, 'undefined');
    assert.equal(window.N, undefined);
    assert.equal(typeof SharedArrayBuffer === 'undefined' || typeof Atomics === 'object', true);
});

// Calls every function and accessor on the DOM prototypes with junk
// receivers and arguments. Exceptions are fine; crashes and hangs are not.
test('every binding survives junk receivers and arguments', () => {
    const doctype = document.doctype, html = document.documentElement, body = document.body;
    const junk = [undefined, null, 0, -1, 2 ** 32, NaN, '', 'x'.repeat(1000), {}, [], () => {}, Symbol('s'),
        document.createTextNode('t'), document.createComment('c'), document, document.createDocumentFragment(),
        document.createElement('div'), window, new Proxy({}, {}), Object.create(Node.prototype)];
    const names = Object.getOwnPropertyNames(window).filter((k) => /^(HTML\w*Element|Node|Element|Document|DocumentFragment|ShadowRoot|CharacterData|Text|Comment|EventTarget|Attr|NamedNodeMap|DOMTokenList|CSSStyleDeclaration|NodeList|HTMLCollection)$/.test(k));
    let calls = 0;
    const skip = new Set(['write', 'writeln', 'open', 'close', 'click', 'focus', 'blur', 'requestSubmit', 'submit',
        'showModal', 'requestClose', 'scrollIntoView', 'reset']);
    for (const name of names) {
        const proto = window[name].prototype;
        for (const key of Object.getOwnPropertyNames(proto)) {
            if (key === 'constructor' || skip.has(key)) continue;
            const d = Object.getOwnPropertyDescriptor(proto, key);
            const fns = [];
            if (typeof d.value === 'function') fns.push(d.value);
            if (d.get) fns.push(d.get);
            if (d.set) fns.push(d.set);
            for (const fn of fns) {
                for (const recv of junk) {
                    for (const arg of [undefined, junk[(calls * 7) % junk.length], junk[(calls * 13) % junk.length]]) {
                        calls++;
                        try {
                            const r = fn.call(recv, arg, arg);
                            if (r instanceof Promise) r.catch(() => {});
                        } catch (e) { /* expected */ }
                    }
                }
            }
        }
    }
    assert.ok(calls > 10000, 'calls ' + calls);
    // The calls may have rearranged the document (document itself is a
    // receiver); put the saved tree back and check it is sound.
    for (const n of [doctype, html, body]) if (n && n.parentNode) n.parentNode.removeChild(n);
    while (document.firstChild) document.removeChild(document.firstChild);
    if (doctype) document.appendChild(doctype);
    document.appendChild(html);
    assert.equal(document.documentElement, html);
    if (!html.contains(body)) html.appendChild(body);
    assert.equal(document.body, body);
});

test('natives reject wrong receivers with TypeError', () => {
    const appendChild = Node.prototype.appendChild;
    assert.throws(() => appendChild.call({}, document.createElement('a')), TypeError);
    const fc = Object.getOwnPropertyDescriptor(Node.prototype, 'firstChild').get;
    assert.throws(() => fc.call(1), TypeError);
    assert.throws(() => Element.prototype.setAttribute.call(document, 'a', 'b'), TypeError);
    assert.throws(() => CharacterData.prototype.appendData.call(document.body, 'x'), TypeError);
});

test('prototype tampering does not break internals', () => {
    const saved = Node.prototype.appendChild;
    Node.prototype.appendChild = () => { throw new Error('tampered'); };
    const d = document.createElement('div');
    let err = null;
    try { d.append('x', document.createElement('i')); } catch (e) { err = e; }
    Node.prototype.appendChild = saved;
    // Either the operation worked or the tampered method ran; the tree is
    // never left inconsistent.
    assert.ok(err === null || err.message === 'tampered');
    assert.ok(d.childNodes.length === 0 || d.childNodes.length === 2);
    Object.defineProperty(Array.prototype, 'evil', { get() { throw new Error('getter'); }, configurable: true });
    document.body.appendChild(document.createElement('p')).remove();
    delete Array.prototype.evil;
});

test('wrappers keep nodes alive when script removes them during callbacks', () => {
    const d = document.createElement('div');
    for (let i = 0; i < 100; i++) d.appendChild(document.createElement('i'));
    document.body.appendChild(d);
    const kids = d.children;
    for (let i = 0; i < kids.length; i++) {
        const k = kids[i];
        k.addEventListener('x', () => { d.textContent = ''; k.remove(); });
        k.dispatchEvent(new Event('x'));
    }
    assert.equal(d.childNodes.length, 0);
    d.remove();
});

test('mutation observer callbacks can tear the tree down', async () => {
    const d = document.createElement('div');
    document.body.appendChild(d);
    const mo = new MutationObserver((recs) => {
        for (const r of recs) for (const n of r.addedNodes) if (n.parentNode) n.parentNode.removeChild(n);
        d.remove();
    });
    mo.observe(d, { childList: true, subtree: true });
    for (let i = 0; i < 20; i++) d.appendChild(document.createElement('b'));
});

test('symbols as weak keys do not leak the realm (patch 0003)', () => {
    globalThis.__wm = new WeakMap();
    globalThis.__sym = Symbol('k');
    __wm.set(__sym, { back: __wm, win: window });
    const wr = new WeakRef(Symbol('r'));
    assert.ok(__wm.has(__sym));
});

test('cookie and storage accessors are strings only', () => {
    assert.equal(typeof document.cookie, 'string');
    document.cookie = { toString() { return 'a=b'; } };
    localStorage.setItem({ toString: () => 'k' }, { toString: () => 'v' });
    assert.equal(localStorage.getItem('k'), 'v');
    localStorage.removeItem('k');
});
