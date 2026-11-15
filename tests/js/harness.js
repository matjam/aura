// A small test harness for tests/js, loaded before each test file by
// `aura-host js tests/js/harness.js FILE.js`. It prints one line per failure
// ("FAIL name: message") and a summary; the runner exits 1 on any FAIL line.
(function () {
    'use strict';
    const results = { pass: 0, fail: 0, pending: 0 };
    const pendingNames = new Set();
    let summaryPrinted = false;

    function fmt(v) {
        try {
            if (typeof v === 'string') return JSON.stringify(v);
            if (v instanceof Node) return '[' + v.nodeName + ']';
            if (Array.isArray(v)) return '[' + v.map(fmt).join(', ') + ']';
            return String(v);
        } catch (e) {
            return '(unprintable)';
        }
    }
    class AssertionError extends Error {}
    const assert = {
        equal(a, b, msg) {
            if (!Object.is(a, b)) throw new AssertionError((msg ? msg + ': ' : '') + 'expected ' + fmt(b) + ', got ' + fmt(a));
        },
        notEqual(a, b, msg) {
            if (Object.is(a, b)) throw new AssertionError((msg ? msg + ': ' : '') + 'expected something other than ' + fmt(b));
        },
        ok(v, msg) {
            if (!v) throw new AssertionError(msg || 'expected a true value, got ' + fmt(v));
        },
        deepEqual(a, b, msg) {
            const sa = JSON.stringify(a), sb = JSON.stringify(b);
            if (sa !== sb) throw new AssertionError((msg ? msg + ': ' : '') + 'expected ' + sb + ', got ' + sa);
        },
        throws(fn, check, msg) {
            let threw = false, err;
            try { fn(); } catch (e) { threw = true; err = e; }
            if (!threw) throw new AssertionError((msg ? msg + ': ' : '') + 'expected an exception');
            if (typeof check === 'string') {
                const name = err && err.name;
                if (name !== check) throw new AssertionError((msg ? msg + ': ' : '') + 'expected ' + check + ', got ' + name + ' (' + (err && err.message) + ')');
            } else if (typeof check === 'function' && !(err instanceof check)) {
                throw new AssertionError((msg ? msg + ': ' : '') + 'wrong exception type: ' + err);
            }
        },
    };

    function pass() { results.pass++; }
    function fail(name, e) {
        results.fail++;
        console.log('FAIL ' + name + ': ' + (e && e.message ? e.message : String(e)) +
            (e && e.stack && !(e instanceof AssertionError) ? '\n' + e.stack : ''));
    }
    function maybeSummary() {
        if (results.pending === 0 && !summaryPrinted && queue.length === 0 && !running) {
            summaryPrinted = true;
            console.log((results.fail ? 'FAILED' : 'PASSED') + ': ' + results.pass + ' passed, ' + results.fail + ' failed');
        }
    }

    // Synchronous tests run at once.
    function test(name, fn) {
        try { fn(); pass(); } catch (e) { fail(name, e); }
    }
    // Asynchronous tests run one after another, each with a timeout (in
    // virtual time, so long timeouts cost nothing).
    const queue = [];
    let running = false;
    function promise_test(name, fn, timeout) {
        queue.push({ name, fn, timeout: timeout || 5000 });
        results.pending++;
        pendingNames.add(name);
        if (!running) setTimeout(next, 0);
    }
    function next() {
        if (running) return;
        const t = queue.shift();
        if (!t) { maybeSummary(); return; }
        running = true;
        let done = false;
        const finish = (err) => {
            if (done) return;
            done = true;
            clearTimeout(timer);
            results.pending--;
            pendingNames.delete(t.name);
            if (err === undefined) pass(); else fail(t.name, err);
            running = false;
            setTimeout(next, 0);
        };
        const timer = setTimeout(() => finish(new Error('timed out')), t.timeout);
        let p;
        try { p = Promise.resolve(t.fn()); } catch (e) { finish(e); return; }
        p.then(() => finish(undefined), (e) => finish(e || new Error('rejected')));
    }
    // Waits for one event.
    function once(target, type) {
        return new Promise((resolve) => target.addEventListener(type, resolve, { once: true }));
    }
    const delay = (ms) => new Promise((r) => setTimeout(r, ms));

    addEventListener('unload', () => {
        for (const n of pendingNames) fail(n, new Error('did not finish'));
        results.pending = 0;
        queue.length = 0;
        running = false;
        maybeSummary();
    });
    setTimeout(maybeSummary, 0);

    Object.assign(globalThis, { test, promise_test, assert, once, delay });
})();
