// EventTarget, Event classes, dispatch phases, handler attributes and
// properties, lifecycle events, mouse events with default actions.

test('dispatch phases and order', () => {
    const outer = document.createElement('div');
    const inner = document.createElement('span');
    outer.appendChild(inner);
    document.body.appendChild(outer);
    const log = [];
    const add = (t, cap, name) => t.addEventListener('x', (e) => log.push(name + ':' + e.eventPhase), cap);
    add(window, true, 'wc'); add(document, true, 'dc'); add(outer, true, 'oc'); add(inner, true, 'ic');
    add(inner, false, 'ib'); add(outer, false, 'ob'); add(document, false, 'db'); add(window, false, 'wb');
    inner.dispatchEvent(new Event('x', { bubbles: true }));
    assert.deepEqual(log, ['wc:1', 'dc:1', 'oc:1', 'ic:2', 'ib:2', 'ob:3', 'db:3', 'wb:3']);
    log.length = 0;
    inner.dispatchEvent(new Event('x'));
    assert.deepEqual(log, ['wc:1', 'dc:1', 'oc:1', 'ic:2', 'ib:2']);
    outer.remove();
});

test('stopPropagation, stopImmediatePropagation, preventDefault', () => {
    const d = document.createElement('div');
    const c = d.appendChild(document.createElement('i'));
    const log = [];
    c.addEventListener('y', (e) => { log.push(1); e.stopImmediatePropagation(); });
    c.addEventListener('y', () => log.push(2));
    d.addEventListener('y', () => log.push(3));
    const e = new Event('y', { bubbles: true, cancelable: true });
    assert.equal(c.dispatchEvent(e), true);
    assert.deepEqual(log, [1]);
    c.addEventListener('z', (e) => e.preventDefault());
    assert.equal(c.dispatchEvent(new Event('z', { cancelable: true })), false);
    assert.equal(c.dispatchEvent(new Event('z')), true);
    const p = new Event('z', { cancelable: true });
    c.addEventListener('z', (e) => assert.equal(e.defaultPrevented, false), { passive: true });
});

test('listener options: once, capture identity, signal, handleEvent', () => {
    const t = new EventTarget();
    let n = 0;
    const f = () => n++;
    t.addEventListener('a', f, { once: true });
    t.dispatchEvent(new Event('a'));
    t.dispatchEvent(new Event('a'));
    assert.equal(n, 1);
    t.addEventListener('b', f);
    t.addEventListener('b', f);
    t.dispatchEvent(new Event('b'));
    assert.equal(n, 2, 'duplicates are ignored');
    t.removeEventListener('b', f);
    t.dispatchEvent(new Event('b'));
    assert.equal(n, 2);
    const ac = new AbortController();
    t.addEventListener('c', f, { signal: ac.signal });
    ac.abort();
    t.dispatchEvent(new Event('c'));
    assert.equal(n, 2);
    const obj = { handleEvent(e) { this.got = e.type; } };
    t.addEventListener('d', obj);
    t.dispatchEvent(new Event('d'));
    assert.equal(obj.got, 'd');
});

test('removing a listener during dispatch', () => {
    const t = new EventTarget();
    const log = [];
    const b = () => log.push('b');
    t.addEventListener('e', () => { log.push('a'); t.removeEventListener('e', b); });
    t.addEventListener('e', b);
    t.dispatchEvent(new Event('e'));
    assert.deepEqual(log, ['a']);
});

test('removing the target during dispatch', () => {
    const d = document.createElement('div');
    const c = d.appendChild(document.createElement('b'));
    document.body.appendChild(d);
    const log = [];
    c.addEventListener('r', () => { d.remove(); c.remove(); log.push('c'); });
    d.addEventListener('r', () => log.push('d'));
    c.dispatchEvent(new Event('r', { bubbles: true }));
    assert.deepEqual(log, ['c', 'd'], 'the path is fixed when dispatch starts');
});

test('event classes', () => {
    const ce = new CustomEvent('c', { detail: { a: 1 } });
    assert.equal(ce.detail.a, 1);
    const me = new MouseEvent('click', { clientX: 5, button: 1, shiftKey: true });
    assert.equal(me.clientX, 5);
    assert.equal(me.button, 1);
    assert.ok(me.shiftKey);
    assert.ok(me instanceof UIEvent);
    const ke = new KeyboardEvent('keydown', { key: 'a', code: 'KeyA' });
    assert.equal(ke.key, 'a');
    assert.equal(new FocusEvent('focus').relatedTarget, null);
    assert.throws(() => new Event(), TypeError);
    assert.equal(new Event('x').isTrusted, false);
    const old = document.createEvent('Event');
    old.initEvent('legacy', true, true);
    assert.equal(old.type, 'legacy');
    assert.ok(old.bubbles);
    assert.throws(() => document.createEvent('NoSuchEvent'), 'NotSupportedError');
});

test('handler attributes compile lazily and see their scope', () => {
    const d = document.createElement('div');
    document.body.appendChild(d);
    window.hits = [];
    d.setAttribute('onclick', 'hits.push(this.localName, event.type, typeof getAttribute)');
    d.dispatchEvent(new Event('click'));
    assert.deepEqual(window.hits, ['div', 'click', 'function']);
    assert.equal(typeof d.onclick, 'function');
    d.setAttribute('onclick', 'return false');
    const e = new Event('click', { cancelable: true });
    d.dispatchEvent(e);
    assert.ok(e.defaultPrevented, 'return false cancels');
    d.onclick = () => hits.push('prop');
    hits.length = 0;
    d.dispatchEvent(new Event('click'));
    assert.deepEqual(hits, ['prop']);
    d.onclick = null;
    hits.length = 0;
    d.dispatchEvent(new Event('click'));
    assert.deepEqual(hits, []);
    let reported = null;
    window.onerror = (msg, src, line, col, err) => { reported = err; return true; };
    d.setAttribute('onmouseover', 'syntax error here(');
    d.dispatchEvent(new Event('mouseover'));
    window.onerror = null;
    assert.ok(reported instanceof SyntaxError, 'a handler that does not compile is reported');
    d.remove();
});

test('handler order follows registration', () => {
    const d = document.createElement('div');
    const log = [];
    d.addEventListener('k', () => log.push(1));
    d.onk = null;
    window.log2 = log;
    d.addEventListener('click', () => log.push('a'));
    d.onclick = () => log.push('h');
    d.addEventListener('click', () => log.push('b'));
    d.dispatchEvent(new Event('click'));
    assert.deepEqual(log, ['a', 'h', 'b']);
});

test('window.onerror sees uncaught listener errors', () => {
    let got = null;
    window.onerror = (msg, src, line, col, err) => { got = err; return true; };
    const t = document.createElement('div');
    t.addEventListener('boom', () => { throw new RangeError('bad'); });
    t.dispatchEvent(new Event('boom'));
    window.onerror = null;
    assert.ok(got instanceof RangeError);
});

test('click() runs activation: checkbox, radio, label', () => {
    const f = document.createElement('form');
    f.innerHTML = '<input type=checkbox id=c><input type=radio name=r id=r1 checked><input type=radio name=r id=r2>' +
        '<label for=c id=l>label</label>';
    document.body.appendChild(f);
    const c = f.querySelector('#c');
    const log = [];
    c.addEventListener('click', () => log.push('click:' + c.checked));
    c.addEventListener('input', () => log.push('input'));
    c.addEventListener('change', () => log.push('change'));
    c.click();
    assert.deepEqual(log, ['click:true', 'input', 'change']);
    assert.ok(c.checked);
    c.addEventListener('click', (e) => e.preventDefault());
    c.click();
    assert.ok(c.checked, 'a cancelled click undoes the toggle');
    const r2 = f.querySelector('#r2');
    r2.click();
    assert.ok(r2.checked);
    assert.ok(!f.querySelector('#r1').checked);
    const l = f.querySelector('#l');
    const before = c.checked;
    let clicked = 0;
    c.addEventListener('click', () => clicked++);
    l.click();
    assert.equal(clicked, 1, 'label forwards the click');
    f.remove();
});

test('submit button submits through requestSubmit and submit events', () => {
    const f = document.createElement('form');
    f.action = 'about:blank';
    f.innerHTML = '<input name=q value="a b"><button>go</button>';
    document.body.appendChild(f);
    let ev = null;
    f.addEventListener('submit', (e) => { ev = e; e.preventDefault(); });
    f.querySelector('button').click();
    assert.ok(ev instanceof SubmitEvent);
    assert.equal(ev.submitter, f.querySelector('button'));
    const fd = new FormData(f);
    assert.equal(fd.get('q'), 'a b');
    f.remove();
});

test('focus and blur events, activeElement', () => {
    const i = document.createElement('input');
    const j = document.createElement('input');
    document.body.append(i, j);
    const log = [];
    i.addEventListener('focus', () => log.push('focus i'));
    i.addEventListener('blur', () => log.push('blur i'));
    document.body.addEventListener('focusin', (e) => log.push('focusin ' + (e.target === i ? 'i' : 'j')));
    i.focus();
    assert.equal(document.activeElement, i);
    j.focus();
    assert.equal(document.activeElement, j);
    assert.deepEqual(log, ['focus i', 'focusin i', 'blur i', 'focusin j']);
    j.blur();
    assert.equal(document.activeElement, document.body);
    const d = document.createElement('div');
    document.body.appendChild(d);
    d.focus();
    assert.equal(document.activeElement, document.body, 'a plain div is not focusable');
    i.remove(); j.remove(); d.remove();
});

const lifecycle = [];
assert.equal(document.readyState, 'interactive');
document.addEventListener('readystatechange', () => lifecycle.push('rs:' + document.readyState));
window.addEventListener('load', (e) => lifecycle.push('load:' + (e.target === document)));
window.addEventListener('pageshow', () => lifecycle.push('pageshow'));
promise_test('lifecycle: readystatechange, load, pageshow', async () => {
    assert.equal(document.readyState, 'complete');
    assert.deepEqual(lifecycle, ['rs:complete', 'load:true', 'pageshow']);
});
