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

test('ElementInternals: form value, validity, reset, states', () => {
    class XField extends HTMLElement {
        static formAssociated = true;
        constructor() { super(); this.i = this.attachInternals(); this.resets = 0; }
        formResetCallback() { this.resets++; }
    }
    customElements.define('x-field', XField);
    class XPlain extends HTMLElement { constructor() { super(); this.i = this.attachInternals(); } }
    customElements.define('x-plain', XPlain);
    const form = document.createElement('form');
    form.innerHTML = '<input name="a" value="1"><x-field name="b"></x-field>';
    document.body.appendChild(form);
    const f = form.querySelector('x-field');
    assert.ok(f.i instanceof ElementInternals);
    assert.throws(() => f.attachInternals(), 'NotSupportedError');
    assert.throws(() => document.createElement('div').attachInternals(), 'NotSupportedError');
    assert.equal(f.i.form, form);
    f.i.setFormValue('two');
    assert.equal([...new FormData(form)].map((e) => e.join('=')).join('&'), 'a=1&b=two');
    const fd = new FormData();
    fd.append('c', '3');
    fd.append('d', '4');
    f.i.setFormValue(fd);
    assert.equal([...new FormData(form)].map((e) => e.join('=')).join('&'), 'a=1&c=3&d=4');
    assert.ok(f.i.willValidate && f.i.validity.valid);
    assert.throws(() => f.i.setValidity({ valueMissing: true }), 'TypeError');
    f.i.setValidity({ valueMissing: true }, 'Fill this in');
    assert.equal(f.i.validationMessage, 'Fill this in');
    let invalid = 0;
    f.addEventListener('invalid', () => invalid++);
    assert.equal(form.checkValidity(), false);
    assert.equal(invalid, 1);
    f.i.setValidity({});
    assert.ok(form.checkValidity());
    form.reset();
    assert.equal(f.resets, 1);
    f.i.states.add('open');
    assert.ok(f.i.states.has('open') && f.i.states.size === 1);
    f.i.ariaLabel = 'x';
    assert.equal(f.i.ariaLabel, 'x');
    const p = document.createElement('x-plain');
    assert.throws(() => p.i.form, 'NotSupportedError');
    f.setAttribute('disabled', '');
    assert.equal(f.i.willValidate, false);
    form.remove();
});
