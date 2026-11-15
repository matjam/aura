// Custom elements: define, upgrade, reactions, createElement, errors.

const log = [];
class XFoo extends HTMLElement {
    static get observedAttributes() { return ['a']; }
    constructor() { super(); this.made = true; log.push('ctor'); }
    connectedCallback() { log.push('connected'); }
    disconnectedCallback() { log.push('disconnected'); }
    attributeChangedCallback(n, o, v) { log.push('attr ' + n + ' ' + o + ' ' + v); }
}

test('elements created before define upgrade on define', () => {
    const early = document.createElement('x-foo');
    early.setAttribute('a', '1');
    document.body.appendChild(early);
    assert.ok(!early.made);
    assert.ok(!(early instanceof XFoo));
    customElements.define('x-foo', XFoo);
    assert.ok(early instanceof XFoo);
    assert.ok(early.made);
    assert.deepEqual(log, ['ctor', 'attr a null 1', 'connected']);
    assert.ok(early.matches(':defined'));
    early.remove();
    log.length = 0;
});

promise_test('reactions run at the end of the DOM call', async () => {
    const e = document.createElement('x-foo');
    assert.deepEqual(log, ['ctor']);
    document.body.appendChild(e);
    e.setAttribute('a', '2');
    e.setAttribute('b', '3');
    e.remove();
    await null;
    assert.deepEqual(log, ['ctor', 'connected', 'attr a null 2', 'disconnected']);
    log.length = 0;
});

test('new and createElement construct synchronously', () => {
    const a = new XFoo();
    assert.equal(a.localName, 'x-foo');
    assert.ok(a.made);
    const b = document.createElement('x-foo');
    assert.ok(b instanceof XFoo);
    assert.equal(customElements.get('x-foo'), XFoo);
    assert.equal(customElements.getName(XFoo), 'x-foo');
    log.length = 0;
});

test('define errors', () => {
    assert.throws(() => customElements.define('nodash', class extends HTMLElement {}), 'SyntaxError');
    assert.throws(() => customElements.define('x-foo', class extends HTMLElement {}), 'NotSupportedError');
    assert.throws(() => customElements.define('x-bar', XFoo), 'NotSupportedError');
    assert.throws(() => customElements.define('font-face', class extends HTMLElement {}), 'SyntaxError');
    assert.throws(() => new (class extends HTMLElement {})(), TypeError);
});

promise_test('whenDefined', async () => {
    const p = customElements.whenDefined('x-later');
    customElements.define('x-later', class extends HTMLElement {});
    const C = await p;
    assert.equal(C, customElements.get('x-later'));
});

test('customized built-ins with is', () => {
    class FancyButton extends HTMLButtonElement { constructor() { super(); this.fancy = true; } }
    customElements.define('fancy-button', FancyButton, { extends: 'button' });
    const b = document.createElement('button', { is: 'fancy-button' });
    assert.ok(b.fancy);
    assert.equal(b.getAttribute('is'), 'fancy-button');
    const parsed = document.createElement('div');
    parsed.innerHTML = '<button is="fancy-button"></button>';
    document.body.appendChild(parsed);
});

promise_test('parsed custom elements upgrade when connected', async () => {
    customElements.define('x-parsed', class extends HTMLElement { connectedCallback() { this.seen = true; } });
    const d = document.createElement('div');
    d.innerHTML = '<x-parsed></x-parsed>';
    document.body.appendChild(d);
    await null;
    assert.ok(d.firstChild.seen);
    d.remove();
});

test('a throwing constructor leaves the element failed and is reported', () => {
    let reported = null;
    window.onerror = (m, s, l, c, e) => { reported = e; return true; };
    const el = document.createElement('x-bad');
    document.body.appendChild(el);
    customElements.define('x-bad', class extends HTMLElement { constructor() { super(); throw new Error('nope'); } });
    window.onerror = null;
    assert.ok(reported && reported.message === 'nope');
    assert.ok(!el.matches(':defined'));
    el.remove();
});
