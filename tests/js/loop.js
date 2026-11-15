// Event loop ordering: tasks, microtasks, timers, nesting clamp, rAF.

const order = [];
setTimeout(() => order.push('timeout 0'), 0);
Promise.resolve().then(() => order.push('promise'));
queueMicrotask(() => order.push('microtask'));
requestAnimationFrame((t) => order.push('raf:' + (typeof t)));
order.push('sync');

promise_test('microtasks run before tasks', async () => {
    await delay(50);
    assert.deepEqual(order.slice(0, 4), ['sync', 'promise', 'microtask', 'timeout 0']);
    assert.ok(order.includes('raf:number'));
});

promise_test('timers run in due order with arguments', async () => {
    const log = [];
    setTimeout((a, b) => log.push('t30' + a + b), 30, '-', 'x');
    setTimeout(() => log.push('t10'), 10);
    setTimeout(() => log.push('t20'), 20);
    const id = setTimeout(() => log.push('cancelled'), 15);
    clearTimeout(id);
    await delay(60);
    assert.deepEqual(log, ['t10', 't20', 't30-x']);
});

promise_test('setInterval repeats until cleared', async () => {
    let n = 0;
    const id = setInterval(() => { if (++n === 3) clearInterval(id); }, 5);
    await delay(100);
    assert.equal(n, 3);
});

promise_test('nested timers are clamped to 4 ms after five levels', async () => {
    const times = [];
    await new Promise((resolve) => {
        let level = 0;
        const step = () => {
            times.push(performance.now());
            if (++level < 10) setTimeout(step, 0); else resolve();
        };
        setTimeout(step, 0);
    });
    const gaps = times.slice(1).map((t, i) => t - times[i]);
    for (let i = 5; i < gaps.length; i++) assert.ok(gaps[i] >= 3.9, 'gap ' + i + ' was ' + gaps[i]);
});

promise_test('string timer bodies are evaluated', async () => {
    window.__strTimer = 0;
    setTimeout('__strTimer = 7', 1);
    await delay(10);
    assert.equal(window.__strTimer, 7);
});

promise_test('microtask checkpoint after each task and listener', async () => {
    const log = [];
    const t = document.createElement('div');
    t.addEventListener('click', () => { Promise.resolve().then(() => log.push('m1')); log.push('l1'); });
    t.addEventListener('click', () => { log.push('l2'); });
    t.click();
    // dispatched from script: microtasks wait until script ends
    log.push('after');
    await null;
    assert.deepEqual(log, ['l1', 'l2', 'after', 'm1']);
});

promise_test('unhandled rejections are reported to the window', async () => {
    let got = null;
    const h = (e) => { got = e.reason; e.preventDefault(); };
    window.addEventListener('unhandledrejection', h);
    Promise.reject(new Error('nobody'));
    await delay(5);
    window.removeEventListener('unhandledrejection', h);
    assert.ok(got instanceof Error && got.message === 'nobody');
});

promise_test('requestAnimationFrame and cancel', async () => {
    let ran = 0;
    const id = requestAnimationFrame(() => ran++);
    cancelAnimationFrame(id);
    await new Promise((r) => requestAnimationFrame(r));
    assert.equal(ran, 0);
});

promise_test('requestIdleCallback', async () => {
    const d = await new Promise((r) => requestIdleCallback(r));
    assert.equal(typeof d.timeRemaining(), 'number');
});

promise_test('long timeouts finish on the virtual clock', async () => {
    await delay(60000);
}, 120000);

test('performance', () => {
    const a = performance.now();
    assert.ok(a >= 0);
    performance.mark('m1');
    const m = performance.measure('x', 'm1');
    assert.ok(m.duration >= 0);
    assert.equal(performance.getEntriesByName('m1').length, 1);
    assert.ok(Math.abs(performance.timeOrigin - Date.now()) < 60000);
});
