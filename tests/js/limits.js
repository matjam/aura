// Limits: memory, stack depth, run time, deep trees. Run with
// --mem 16 --kill 4000 --expect-errors 5: two scripts run past the time
// limit and two run out of memory, each stopped and reported once; one
// throws null.

test('stack overflow is a catchable error', () => {
    let depth = 0;
    const f = () => { depth++; f(); };
    let err = null;
    try { f(); } catch (e) { err = e; }
    assert.ok(err instanceof RangeError, String(err));
    assert.ok(depth > 100, 'depth ' + depth);
    // after unwinding, calls work again
    assert.equal([1, 2, 3].map((x) => x * 2).join(), '2,4,6');
});

test('deep trees do not recurse in C', () => {
    const top = document.createElement('div');
    let cur = top;
    for (let i = 0; i < 5000; i++) cur = cur.appendChild(document.createElement('div'));
    cur.textContent = 'leaf';
    assert.equal(top.textContent, 'leaf');
    const c = top.cloneNode(true);
    assert.equal(c.textContent, 'leaf');
    assert.ok(top.innerHTML.length > 5000 * 11);
    assert.ok(top.isEqualNode(c));
    document.body.appendChild(top);
    assert.ok(document.querySelectorAll('div').length >= 5001);
    top.remove();
    const d = document.createElement('div');
    d.innerHTML = '<b>'.repeat(5000) + 'x';
    assert.equal(d.textContent, 'x');
});

// Running out of memory stops the script (QuickJS makes InternalError
// uncatchable, so the script cannot keep allocating); the page goes on.
window.__after = [];
setTimeout(() => {
    const keep = [];
    for (let i = 0; i < 1e6; i++) keep.push(new Array(100000).fill(i));
}, 0);
setTimeout(() => {
    const d = document.createElement('div');
    d.textContent = 'alive';
    window.__after.push(d.textContent);
}, 5);
// Many small allocations up to the limit: the out-of-memory error itself
// must still be built (it once came through as a catchable null).
setTimeout(() => {
    window.__caught = 'nothing';
    try {
        const parts = [];
        for (let i = 0; i < 1e7; i++) parts.push('item' + i);
    } catch (e) {
        window.__caught = e;
    }
}, 7);
setTimeout(() => { throw null; }, 8); /* reported as "Uncaught null" */
setTimeout(() => { for (;;) {} }, 10);
setTimeout(() => window.__after.push('after kill'), 15);
setTimeout(() => { for (;;) { [].concat([1]); } }, 20);

promise_test('the page keeps running after stopped scripts', async () => {
    await delay(50);
    assert.deepEqual(window.__after, ['alive', 'after kill']);
    assert.equal(window.__caught, 'nothing', 'out of memory is not catchable');
}, 60000);
