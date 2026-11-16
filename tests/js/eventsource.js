// EventSource (src/js/eventsource.js) over data: URLs, which give a whole
// event stream and end, so every source here also reconnects. The parser
// itself is tested in C (tests/net/test_sse.c); a server that sends events
// slowly, Last-Event-ID and CORS are in tests/webapi/test_streams_net.c.

const sse = (body) => 'data:text/event-stream,' + encodeURIComponent(body);

test('EventSource is a lazy global with the constants', () => {
    assert.equal(typeof EventSource, 'function');
    const d = Object.getOwnPropertyDescriptor(globalThis, 'EventSource');
    assert.equal(typeof d.value, 'function');
    assert.equal(d.enumerable, false);
    assert.equal(EventSource.CONNECTING, 0);
    assert.equal(EventSource.OPEN, 1);
    assert.equal(EventSource.CLOSED, 2);
    assert.equal(EventSource.prototype.OPEN, 1);
    assert.throws(() => new EventSource(), 'TypeError');
    assert.throws(() => EventSource('x'), 'TypeError');
    assert.throws(() => new EventSource('http://[bad'), 'SyntaxError');
    assert.throws(() => EventSource.prototype.readyState, 'TypeError');
    const es = new EventSource(sse(''));
    assert.ok(es instanceof EventTarget);
    assert.equal(es.readyState, 0);
    assert.equal(es.withCredentials, false);
    assert.equal(es.url, sse(''));
    assert.equal(Object.prototype.toString.call(es), '[object EventSource]');
    assert.equal(es.onmessage, null);
    es.close();
    assert.equal(es.readyState, 2);
    assert.equal(new EventSource(sse(''), { withCredentials: true }).withCredentials, true);
});

promise_test('open, messages with types, ids and data lines, then a reconnection', async () => {
    const es = new EventSource(sse(':comment\nretry: 50\n\ndata: one\n\nevent: ping\ndata: a\ndata: b\nid: 7\n\ndata: no end'));
    const got = [];
    let opened = 0;
    es.onopen = () => { opened++; assert.equal(es.readyState, 1); };
    es.onmessage = (e) => got.push(['message', e.data, e.lastEventId]);
    es.addEventListener('ping', (e) => got.push(['ping', e.data, e.lastEventId, e instanceof MessageEvent]));
    await once(es, 'error');
    assert.equal(opened, 1);
    assert.equal(es.readyState, 0, 'a stream that ends reconnects');
    assert.deepEqual(got, [['message', 'one', ''], ['ping', 'a\nb', '7', true]]);
    await once(es, 'open');
    assert.equal(opened, 2, 'connected again after the retry time');
    es.close();
    assert.equal(es.readyState, 2);
});

promise_test('the origin of a message event', async () => {
    const es = new EventSource(sse('data: x\n\n'));
    const e = await once(es, 'message');
    assert.equal(typeof e.origin, 'string');
    assert.equal(e.data, 'x');
    es.close();
});

promise_test('close in a handler stops the events after it', async () => {
    const es = new EventSource(sse('data: 1\n\ndata: 2\n\n'));
    const got = [];
    es.onmessage = (e) => { got.push(e.data); es.close(); };
    let errors = 0;
    es.onerror = () => errors++;
    await delay(200);
    assert.deepEqual(got, ['1']);
    assert.equal(errors, 0);
    assert.equal(es.readyState, 2);
});

promise_test('a response that is not text/event-stream fails the connection', async () => {
    const es = new EventSource('data:text/plain,data%3A%20x%0A%0A');
    let opened = false;
    es.onopen = () => { opened = true; };
    await once(es, 'error');
    assert.ok(!opened);
    assert.equal(es.readyState, 2, 'closed for good');
    await delay(5000);
    assert.equal(es.readyState, 2);
}, 20000);

promise_test('a refused scheme fails the connection', async () => {
    const es = new EventSource('ftp://example.test/');
    assert.equal(es.readyState, 0, 'the failure is a task');
    await once(es, 'error');
    assert.equal(es.readyState, 2);
});

promise_test('more than four sources wait for a free connection', async () => {
    const list = [];
    for (let i = 0; i < 6; i++) list.push(new EventSource(sse('retry: 100000\ndata: ' + i + '\n\n')));
    const got = [];
    for (const es of list) es.onmessage = (e) => got.push(e.data);
    await delay(100);
    assert.deepEqual(got.slice().sort(), ['0', '1', '2', '3', '4', '5'], 'each ends and frees a slot for the next');
    for (const es of list) es.close();
});

promise_test('EventSource in a worker', async () => {
    const src = 'const es = new EventSource(' + JSON.stringify(sse('data: from worker\n\n')) +
        '); es.onmessage = (e) => { postMessage([e.data, es.readyState, typeof ReadableStream]); es.close(); };';
    const w = new Worker(URL.createObjectURL(new Blob([src], { type: 'text/javascript' })));
    const e = await once(w, 'message');
    assert.deepEqual(e.data, ['from worker', 1, 'function']);
    w.terminate();
});

promise_test('a worker reads a fetch body as a stream', async () => {
    const src = 'fetch("data:text/plain,abc").then((r) => r.body.getReader().read()).then((x) => postMessage(Array.from(x.value)));';
    const w = new Worker(URL.createObjectURL(new Blob([src], { type: 'text/javascript' })));
    const e = await once(w, 'message');
    assert.deepEqual(e.data, [97, 98, 99]);
    w.terminate();
});
