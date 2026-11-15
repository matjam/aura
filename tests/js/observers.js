// MutationObserver, IntersectionObserver, ResizeObserver.

promise_test('MutationObserver childList, attributes, characterData', async () => {
    const d = document.createElement('div');
    document.body.appendChild(d);
    const recs = [];
    const mo = new MutationObserver((r, o) => { assert.equal(o, mo); recs.push(...r); });
    mo.observe(d, { childList: true, attributes: true, characterData: true, subtree: true, attributeOldValue: true,
        characterDataOldValue: true });
    d.setAttribute('a', '1');
    d.setAttribute('a', '2');
    const t = d.appendChild(document.createTextNode('x'));
    t.data = 'y';
    const f = document.createDocumentFragment();
    f.append(document.createElement('i'), document.createElement('b'));
    d.appendChild(f);
    d.removeChild(t);
    assert.equal(recs.length, 0, 'delivery waits for a microtask');
    await null;
    assert.deepEqual(recs.map((r) => r.type), ['attributes', 'attributes', 'childList', 'characterData', 'childList', 'childList']);
    assert.equal(recs[0].attributeName, 'a');
    assert.equal(recs[0].oldValue, null);
    assert.equal(recs[1].oldValue, '1');
    assert.equal(recs[3].oldValue, 'x');
    assert.equal(recs[4].addedNodes.length, 2, 'a fragment insert is one record');
    assert.equal(recs[5].removedNodes[0], t);
    assert.ok(recs[0] instanceof MutationRecord);
    mo.disconnect();
    d.setAttribute('b', '1');
    await null;
    assert.equal(recs.length, 6);
    d.remove();
});

promise_test('MutationObserver attributeFilter, subtree off, takeRecords', async () => {
    const d = document.createElement('div');
    const c = d.appendChild(document.createElement('span'));
    let calls = 0;
    const mo = new MutationObserver(() => calls++);
    mo.observe(d, { attributeFilter: ['x'] });
    d.setAttribute('y', '1');
    c.setAttribute('x', '1');
    d.setAttribute('x', '1');
    const taken = mo.takeRecords();
    assert.equal(taken.length, 1);
    assert.equal(taken[0].attributeName, 'x');
    await null;
    assert.equal(calls, 0, 'taken records are not delivered');
    assert.throws(() => mo.observe(d, {}), TypeError);
    assert.throws(() => new MutationObserver(1), TypeError);
});

promise_test('MutationObserver callback mutating the DOM gets a new delivery', async () => {
    const d = document.createElement('div');
    let n = 0;
    const mo = new MutationObserver(() => { if (++n < 3) d.appendChild(document.createElement('p')); });
    mo.observe(d, { childList: true });
    d.appendChild(document.createElement('p'));
    await delay(1);
    assert.equal(n, 3);
    mo.disconnect();
});

promise_test('IntersectionObserver reports an initial entry', async () => {
    const d = document.createElement('div');
    document.body.appendChild(d);
    const entries = await new Promise((resolve) => {
        const io = new IntersectionObserver((e) => { io.disconnect(); resolve(e); }, { threshold: [0, 1] });
        io.observe(d);
    });
    assert.equal(entries.length, 1);
    assert.equal(entries[0].target, d);
    assert.equal(typeof entries[0].isIntersecting, 'boolean');
    assert.throws(() => new IntersectionObserver(() => {}, { threshold: 2 }), RangeError);
    assert.throws(() => new IntersectionObserver(() => {}, { rootMargin: 'x' }), 'SyntaxError');
    d.remove();
});

promise_test('ResizeObserver reports an initial entry', async () => {
    const d = document.createElement('div');
    document.body.appendChild(d);
    const entries = await new Promise((resolve) => {
        const ro = new ResizeObserver((e) => { ro.disconnect(); resolve(e); });
        ro.observe(d);
    });
    assert.equal(entries[0].target, d);
    assert.equal(typeof entries[0].contentRect.width, 'number');
    assert.equal(entries[0].borderBoxSize.length, 1);
    d.remove();
});
