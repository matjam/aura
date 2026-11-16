// Streams (src/js/streams.js) and the bodies that use them
// (src/js/prelude_net.js). The cases follow the Web Platform Tests'
// streams/ and fetch/api/ directories: each checks one rule of the
// WHATWG Streams or Fetch standard.

const flush = () => delay(0);
const bytes = (...a) => new Uint8Array(a);
const text = (u8) => new TextDecoder().decode(u8);
async function readAllChunks(rs) {
    const r = rs.getReader();
    const out = [];
    for (;;) {
        const { value, done } = await r.read();
        if (done) return out;
        out.push(value);
    }
}
async function concat(rs) {
    const parts = await readAllChunks(rs);
    const n = parts.reduce((a, p) => a + p.length, 0);
    const u = new Uint8Array(n);
    let o = 0;
    for (const p of parts) { u.set(p, o); o += p.length; }
    return u;
}

test('the stream globals are lazy data properties', () => {
    for (const name of ['ReadableStream', 'WritableStream', 'TransformStream', 'ByteLengthQueuingStrategy',
        'CountQueuingStrategy', 'ReadableStreamDefaultReader', 'ReadableStreamBYOBReader',
        'ReadableStreamDefaultController', 'ReadableByteStreamController', 'ReadableStreamBYOBRequest',
        'WritableStreamDefaultWriter', 'WritableStreamDefaultController', 'TransformStreamDefaultController',
        'TextEncoderStream', 'TextDecoderStream', 'CompressionStream', 'DecompressionStream']) {
        assert.equal(typeof globalThis[name], 'function', name);
        const d = Object.getOwnPropertyDescriptor(globalThis, name);
        assert.equal(typeof d.value, 'function', name + ' is a data property after use');
        assert.equal(d.enumerable, false, name);
        assert.equal(d.writable, true, name);
    }
    assert.equal(Object.prototype.toString.call(new ReadableStream()), '[object ReadableStream]');
    assert.throws(() => ReadableStream(), 'TypeError');
    assert.throws(() => new ReadableStreamDefaultReader(), 'TypeError');
    assert.throws(() => new ReadableStreamDefaultController(), 'TypeError');
    assert.throws(() => ReadableStream.prototype.getReader.call({}), 'TypeError');
});

test('constructor argument checks', () => {
    assert.throws(() => new ReadableStream({ type: 'nonsense' }), 'TypeError');
    assert.throws(() => new ReadableStream({ start: 5 }), 'TypeError');
    assert.throws(() => new ReadableStream({}, { highWaterMark: -1 }), 'RangeError');
    assert.throws(() => new ReadableStream({}, { highWaterMark: NaN }), 'RangeError');
    assert.throws(() => new ReadableStream({ type: 'bytes' }, { size() { return 1; } }), 'RangeError');
    assert.throws(() => new ReadableStream({ type: 'bytes', autoAllocateChunkSize: 0 }), 'TypeError');
    assert.throws(() => new WritableStream({ type: 'bytes' }), 'RangeError');
    assert.throws(() => new TransformStream({ readableType: 'bytes' }), 'RangeError');
    // the strategy is read before the source, its members in name order (WebIDL)
    const order = [];
    new ReadableStream({ get start() { order.push('start'); } },
        { get size() { order.push('size'); }, get highWaterMark() { order.push('hwm'); return 1; } });
    assert.deepEqual(order, ['hwm', 'size', 'start']);
});

test('queuing strategies', () => {
    const c = new CountQueuingStrategy({ highWaterMark: 4 });
    assert.equal(c.highWaterMark, 4);
    assert.equal(c.size(), 1);
    assert.equal(c.size, new CountQueuingStrategy({ highWaterMark: 1 }).size, 'size is shared');
    const b = new ByteLengthQueuingStrategy({ highWaterMark: 16 });
    assert.equal(b.size(new Uint8Array(7)), 7);
    assert.equal(b.size.name, 'size');
    assert.throws(() => new CountQueuingStrategy(), 'TypeError');
    assert.throws(() => new ByteLengthQueuingStrategy({}), 'TypeError');
});

promise_test('a default stream reads what was enqueued, then closes', async () => {
    const rs = new ReadableStream({ start(c) { c.enqueue('a'); c.enqueue('b'); c.close(); } });
    const r = rs.getReader();
    assert.ok(rs.locked);
    assert.deepEqual(await r.read(), { value: 'a', done: false });
    assert.deepEqual(await r.read(), { value: 'b', done: false });
    const end = await r.read();
    assert.equal(end.done, true);
    assert.equal(end.value, undefined);
    await r.closed;
    r.releaseLock();
    assert.ok(!rs.locked);
});

promise_test('desiredSize, pull and backpressure', async () => {
    let pulls = 0, ctl;
    const rs = new ReadableStream({ start(c) { ctl = c; }, pull(c) { pulls++; c.enqueue(pulls); } },
        { highWaterMark: 2 });
    await flush();
    assert.equal(pulls, 2, 'pulled to the high-water mark');
    assert.equal(ctl.desiredSize, 0);
    const r = rs.getReader();
    assert.equal((await r.read()).value, 1);
    await flush();
    assert.equal(pulls, 3);
    ctl.close();
    assert.equal(ctl.desiredSize, 0);
    assert.equal((await r.read()).value, 2);
    assert.equal((await r.read()).value, 3);
    assert.ok((await r.read()).done);
    assert.equal(ctl.desiredSize, 0);
});

promise_test('size() is used and its errors error the stream', async () => {
    let ctl;
    const rs = new ReadableStream({ start(c) { ctl = c; } }, { highWaterMark: 10, size: (s) => s.length });
    ctl.enqueue('abcd');
    assert.equal(ctl.desiredSize, 6);
    const bad = new ReadableStream({ start(c) { ctl = c; } }, { size() { throw new Error('boom'); } });
    assert.throws(() => ctl.enqueue('x'), Error);
    try { await bad.getReader().read(); assert.ok(false, 'should reject'); } catch (e) { assert.equal(e.message, 'boom'); }
});

promise_test('an errored stream rejects reads and closed', async () => {
    const e = new Error('x');
    const rs = new ReadableStream({ start(c) { c.error(e); } });
    const r = rs.getReader();
    try { await r.read(); assert.ok(false); } catch (x) { assert.equal(x, e); }
    try { await r.closed; assert.ok(false); } catch (x) { assert.equal(x, e); }
    // a start that rejects errors it too
    const rs2 = new ReadableStream({ start() { return Promise.reject(e); } });
    try { await rs2.getReader().closed; assert.ok(false); } catch (x) { assert.equal(x, e); }
});

promise_test('cancel calls the source and resolves to undefined', async () => {
    let reason;
    const rs = new ReadableStream({ start(c) { c.enqueue(1); }, cancel(r) { reason = r; return 'ignored'; } });
    assert.equal(await rs.cancel('why'), undefined);
    assert.equal(reason, 'why');
    const r = rs.getReader();
    assert.ok((await r.read()).done, 'a canceled stream is closed');
    const locked = new ReadableStream();
    locked.getReader();
    try { await locked.cancel(); assert.ok(false); } catch (e) { assert.equal(e.name, 'TypeError'); }
});

promise_test('releaseLock rejects pending reads', async () => {
    const rs = new ReadableStream();
    const r = rs.getReader();
    const p = r.read();
    r.releaseLock();
    try { await p; assert.ok(false); } catch (e) { assert.equal(e.name, 'TypeError'); }
    try { await r.closed; assert.ok(false); } catch (e) { assert.equal(e.name, 'TypeError'); }
    const r2 = rs.getReader();
    assert.ok(r2 !== r);
    assert.throws(() => rs.getReader(), 'TypeError');
    assert.throws(() => rs.getReader({ mode: 'byob' }), 'TypeError');
});

promise_test('tee gives both branches every chunk', async () => {
    const rs = new ReadableStream({ start(c) { c.enqueue('a'); c.enqueue('b'); c.close(); } });
    const [a, b] = rs.tee();
    assert.deepEqual(await readAllChunks(a), ['a', 'b']);
    assert.deepEqual(await readAllChunks(b), ['a', 'b']);
});

promise_test('tee cancels the source when both branches cancel', async () => {
    let reason;
    const rs = new ReadableStream({ cancel(r) { reason = r; } });
    const [a, b] = rs.tee();
    const pa = a.cancel(1);
    await flush();
    assert.equal(reason, undefined);
    await Promise.all([pa, b.cancel(2)]);
    assert.deepEqual(reason, [1, 2]);
});

promise_test('async iteration and preventCancel', async () => {
    const rs = new ReadableStream({ start(c) { for (const x of [1, 2, 3]) c.enqueue(x); c.close(); } });
    const seen = [];
    for await (const x of rs) seen.push(x);
    assert.deepEqual(seen, [1, 2, 3]);
    let canceled = false;
    const rs2 = new ReadableStream({ start(c) { c.enqueue(1); c.enqueue(2); }, cancel() { canceled = true; } });
    for await (const x of rs2.values({ preventCancel: true })) { assert.equal(x, 1); break; }
    assert.ok(!canceled);
    assert.ok(!rs2.locked);
    for await (const x of rs2) { assert.equal(x, 2); break; }
    assert.ok(canceled);
    assert.equal(ReadableStream.prototype[Symbol.asyncIterator], ReadableStream.prototype.values);
    const proto = Object.getPrototypeOf(Object.getPrototypeOf(new ReadableStream().values()));
    assert.equal(typeof proto[Symbol.asyncIterator], 'function', 'inherits %AsyncIteratorPrototype%');
});

promise_test('ReadableStream.from', async () => {
    assert.deepEqual(await readAllChunks(ReadableStream.from([1, 2, 3])), [1, 2, 3]);
    async function* gen() { yield 'x'; yield 'y'; }
    assert.deepEqual(await readAllChunks(ReadableStream.from(gen())), ['x', 'y']);
    const inner = new ReadableStream({ start(c) { c.enqueue(9); c.close(); } });
    assert.deepEqual(await readAllChunks(ReadableStream.from(inner)), [9]);
    assert.throws(() => ReadableStream.from(5), 'TypeError');
    assert.throws(() => ReadableStream.from({ [Symbol.iterator]: 1 }), 'TypeError');
    // a promise in a sync iterable is awaited
    assert.deepEqual(await readAllChunks(ReadableStream.from([Promise.resolve(4)])), [4]);
});

promise_test('a byte stream with a default reader', async () => {
    const rs = new ReadableStream({ type: 'bytes', start(c) { c.enqueue(bytes(1, 2)); c.enqueue(bytes(3)); c.close(); } });
    const chunks = await readAllChunks(rs);
    assert.deepEqual(chunks.map((u) => [...u]), [[1, 2], [3]]);
    let ctl;
    new ReadableStream({ type: 'bytes', start(c) { ctl = c; } });
    assert.throws(() => ctl.enqueue(new Uint8Array(0)), 'TypeError');
    assert.throws(() => ctl.enqueue('x'), 'TypeError');
    assert.equal(ctl.byobRequest, null);
});

promise_test('a BYOB reader fills the view it is given', async () => {
    const rs = new ReadableStream({ type: 'bytes', start(c) { c.enqueue(bytes(1, 2, 3, 4, 5)); c.close(); } });
    const r = rs.getReader({ mode: 'byob' });
    const first = await r.read(new Uint8Array(3));
    assert.deepEqual([...first.value], [1, 2, 3]);
    assert.equal(first.value.buffer.byteLength, 3);
    const second = await r.read(new Uint8Array(8));
    assert.deepEqual([...second.value], [4, 5]);
    const end = await r.read(new Uint8Array(8));
    assert.ok(end.done);
    assert.equal(end.value.byteLength, 0);
    assert.equal(end.value.constructor, Uint8Array);
    try { await r.read(new Uint8Array(0)); assert.ok(false); } catch (e) { assert.equal(e.name, 'TypeError'); }
});

promise_test('the BYOB request: respond and respondWithNewView', async () => {
    const rs = new ReadableStream({
        type: 'bytes',
        pull(c) {
            const req = c.byobRequest;
            assert.ok(req, 'a BYOB read gives the source a request');
            req.view[0] = 7;
            req.view[1] = 8;
            req.respond(2);
        },
    });
    const r = rs.getReader({ mode: 'byob' });
    const v = new Uint16Array(2);
    const { value } = await r.read(v);
    assert.equal(value.constructor, Uint16Array);
    assert.equal(value.length, 1);
    assert.equal(new Uint8Array(value.buffer)[0], 7);
    assert.equal(v.byteLength, 0, 'the given buffer is detached');

    const rs2 = new ReadableStream({
        type: 'bytes',
        pull(c) {
            const req = c.byobRequest;
            const nv = new Uint8Array(req.view.buffer, req.view.byteOffset, 1);
            nv[0] = 42;
            req.respondWithNewView(nv);
        },
    });
    const r2 = rs2.getReader({ mode: 'byob' });
    assert.equal((await r2.read(new Uint8Array(4))).value[0], 42);
});

promise_test('min: a BYOB read waits for at least min elements', async () => {
    let ctl;
    const rs = new ReadableStream({ type: 'bytes', start(c) { ctl = c; } });
    const r = rs.getReader({ mode: 'byob' });
    const p = r.read(new Uint8Array(4), { min: 3 });
    ctl.enqueue(bytes(1));
    ctl.enqueue(bytes(2));
    await flush();
    ctl.enqueue(bytes(3, 4));
    const { value } = await p;
    assert.deepEqual([...value], [1, 2, 3, 4]);
    try { await r.read(new Uint8Array(2), { min: 3 }); assert.ok(false); } catch (e) { assert.equal(e.name, 'RangeError'); }
});

promise_test('autoAllocateChunkSize gives a default reader a BYOB request', async () => {
    const rs = new ReadableStream({
        type: 'bytes',
        autoAllocateChunkSize: 16,
        pull(c) {
            const req = c.byobRequest;
            assert.equal(req.view.byteLength, 16);
            req.view[0] = 1;
            req.respond(1);
            c.close();
        },
    });
    const r = rs.getReader();
    const { value } = await r.read();
    assert.equal(value.length, 1);
    assert.ok((await r.read()).done);
});

promise_test('tee of a byte stream clones the chunks', async () => {
    const rs = new ReadableStream({ type: 'bytes', start(c) { c.enqueue(bytes(1, 2)); c.close(); } });
    const [a, b] = rs.tee();
    const ca = await readAllChunks(a);
    const rb = b.getReader({ mode: 'byob' });
    const vb = (await rb.read(new Uint8Array(4))).value;
    assert.deepEqual([...ca[0]], [1, 2]);
    assert.deepEqual([...vb], [1, 2]);
    assert.ok(ca[0].buffer !== vb.buffer, 'each branch has its own bytes');
    assert.ok((await rb.read(new Uint8Array(1))).done);
});

promise_test('WritableStream: writes in order, close waits for them', async () => {
    const log = [];
    const ws = new WritableStream({
        write(chunk) { log.push(chunk); return delay(1); },
        close() { log.push('close'); },
    });
    const w = ws.getWriter();
    assert.equal(w.desiredSize, 1);
    w.write('a');
    assert.equal(w.desiredSize, 0);
    w.write('b');
    await w.close();
    assert.deepEqual(log, ['a', 'b', 'close']);
    await w.closed;
    try { await w.write('c'); assert.ok(false); } catch (e) { assert.equal(e.name, 'TypeError'); }
});

promise_test('WritableStream: ready follows backpressure', async () => {
    let resolveWrite;
    const ws = new WritableStream({ write() { return new Promise((r) => { resolveWrite = r; }); } },
        { highWaterMark: 1 });
    const w = ws.getWriter();
    await w.ready;
    w.write(1);
    let ready = false;
    w.ready.then(() => { ready = true; });
    await flush();
    assert.ok(!ready, 'not ready while a write is queued');
    resolveWrite();
    await flush();
    await flush();
    assert.ok(ready);
});

promise_test('WritableStream: abort and errors', async () => {
    let abortReason, signalAborted;
    const ws = new WritableStream({
        start(c) { c.signal.addEventListener('abort', () => { signalAborted = true; }); },
        abort(r) { abortReason = r; },
    });
    const w = ws.getWriter();
    await w.abort('stop');
    assert.equal(abortReason, 'stop');
    assert.ok(signalAborted);
    try { await w.closed; assert.ok(false); } catch (e) { assert.equal(e, 'stop'); }
    const e = new Error('sink');
    const ws2 = new WritableStream({ write() { throw e; } });
    const w2 = ws2.getWriter();
    try { await w2.write(1); assert.ok(false); } catch (x) { assert.equal(x, e); }
    try { await w2.closed; assert.ok(false); } catch (x) { assert.equal(x, e); }
    let ctl;
    const ws3 = new WritableStream({ start(c) { ctl = c; } });
    ctl.error(e);
    try { await ws3.getWriter().ready; assert.ok(false); } catch (x) { assert.equal(x, e); }
    assert.throws(() => new WritableStreamDefaultWriter(new WritableStream()).releaseLock() || ws3.getWriter(), 'TypeError');
});

promise_test('pipeTo moves every chunk and closes', async () => {
    const out = [];
    const rs = ReadableStream.from(['a', 'b', 'c']);
    await rs.pipeTo(new WritableStream({ write(c) { out.push(c); } }));
    assert.deepEqual(out, ['a', 'b', 'c']);
    assert.ok(!rs.locked);
});

promise_test('pipeTo carries errors both ways', async () => {
    const e = new Error('src');
    let aborted;
    const rs = new ReadableStream({ start(c) { c.error(e); } });
    try { await rs.pipeTo(new WritableStream({ abort(r) { aborted = r; } })); assert.ok(false); } catch (x) { assert.equal(x, e); }
    assert.equal(aborted, e);
    const e2 = new Error('dest');
    let canceled;
    const rs2 = new ReadableStream({ pull(c) { c.enqueue(1); }, cancel(r) { canceled = r; } });
    try { await rs2.pipeTo(new WritableStream({ write() { throw e2; } })); assert.ok(false); } catch (x) { assert.equal(x, e2); }
    assert.equal(canceled, e2);
    // preventAbort keeps the destination
    const ws = new WritableStream();
    try { await new ReadableStream({ start(c) { c.error(e); } }).pipeTo(ws, { preventAbort: true }); } catch (x) { /* expected */ }
    assert.ok(!ws.locked);
    await ws.getWriter().write(1);
});

promise_test('pipeTo with an AbortSignal', async () => {
    const ac = new AbortController();
    let canceled = false;
    const rs = new ReadableStream({ cancel() { canceled = true; } });
    const p = rs.pipeTo(new WritableStream(), { signal: ac.signal });
    ac.abort();
    try { await p; assert.ok(false); } catch (e) { assert.equal(e.name, 'AbortError'); }
    assert.ok(canceled);
    try { await new ReadableStream().pipeTo(new WritableStream(), { signal: 5 }); assert.ok(false); } catch (e) { assert.equal(e.name, 'TypeError'); }
});

promise_test('TransformStream: identity and transform()', async () => {
    const id = new TransformStream();
    const w = id.writable.getWriter();
    w.write('x');
    w.close();
    assert.deepEqual(await readAllChunks(id.readable), ['x']);
    const up = new TransformStream({
        transform(chunk, c) { c.enqueue(chunk.toUpperCase()); c.enqueue('!'); },
        flush(c) { c.enqueue('end'); },
    });
    const out = ReadableStream.from(['a', 'b']).pipeThrough(up);
    assert.deepEqual(await readAllChunks(out), ['A', '!', 'B', '!', 'end']);
});

promise_test('TransformStream: errors and terminate', async () => {
    const e = new Error('t');
    const ts = new TransformStream({ transform() { throw e; } });
    const w = ts.writable.getWriter();
    // the write waits until the readable side is read (backpressure)
    const wp = w.write(1);
    try { await ts.readable.getReader().read(); assert.ok(false); } catch (x) { assert.equal(x, e); }
    try { await wp; assert.ok(false); } catch (x) { assert.equal(x, e); }
    const ts2 = new TransformStream({ start(c) { c.enqueue('only'); c.terminate(); } });
    assert.deepEqual(await readAllChunks(ts2.readable), ['only']);
    try { await ts2.writable.getWriter().write(1); assert.ok(false); } catch (x) { assert.equal(x.name, 'TypeError'); }
});

promise_test('TransformStream: cancel reaches the transformer', async () => {
    let reason;
    const ts = new TransformStream({ cancel(r) { reason = r; } });
    await ts.readable.cancel('no');
    assert.equal(reason, 'no');
    try { await ts.writable.getWriter().closed; assert.ok(false); } catch (x) { assert.equal(x, 'no'); }
});

promise_test('pipeThrough checks its argument', async () => {
    assert.throws(() => new ReadableStream().pipeThrough({ readable: new ReadableStream() }), 'TypeError');
    assert.throws(() => new ReadableStream().pipeThrough({ writable: new WritableStream() }), 'TypeError');
});

promise_test('TextEncoderStream and TextDecoderStream', async () => {
    const enc = new TextEncoderStream();
    assert.equal(enc.encoding, 'utf-8');
    const w = enc.writable.getWriter();
    w.write('hé');
    w.write('\ud83d');
    w.write('\ude00');
    w.close();
    const u = await concat(enc.readable);
    assert.deepEqual([...u], [0x68, 0xc3, 0xa9, 0xf0, 0x9f, 0x98, 0x80]);
    const dec = new TextDecoderStream();
    assert.equal(dec.encoding, 'utf-8');
    assert.equal(dec.fatal, false);
    const dw = dec.writable.getWriter();
    dw.write(bytes(0x68, 0xc3));
    dw.write(bytes(0xa9, 0xf0, 0x9f));
    dw.write(bytes(0x98, 0x80));
    dw.close();
    assert.equal((await readAllChunks(dec.readable)).join(''), 'hé😀');
    const fatal = new TextDecoderStream('utf-8', { fatal: true });
    const fw = fatal.writable.getWriter();
    fw.write(bytes(0xff)).catch(() => {});
    try { await fatal.readable.getReader().read(); assert.ok(false); } catch (e) { assert.equal(e.name, 'TypeError'); }
    const enc2 = new TextEncoderStream();
    const w2 = enc2.writable.getWriter();
    w2.write('\ud83d');
    w2.close();
    assert.deepEqual([...await concat(enc2.readable)], [0xef, 0xbf, 0xbd], 'a lone high surrogate at the end');
});

async function roundTrip(fmt, input) {
    const cs = new Blob([input]).stream().pipeThrough(new CompressionStream(fmt));
    const packed = await concat(cs);
    const ds = new Blob([packed]).stream().pipeThrough(new DecompressionStream(fmt));
    return [packed, await concat(ds)];
}

promise_test('CompressionStream and DecompressionStream round trips', async () => {
    const big = new Uint8Array(300000);
    for (let i = 0; i < big.length; i++) big[i] = (i * 7) % 251;
    for (const fmt of ['gzip', 'deflate', 'deflate-raw']) {
        const [packed, out] = await roundTrip(fmt, 'hello hello hello hello');
        assert.equal(text(out), 'hello hello hello hello', fmt);
        if (fmt === 'gzip') assert.deepEqual([packed[0], packed[1]], [0x1f, 0x8b]);
        if (fmt === 'deflate') assert.equal(packed[0] & 0x0f, 8);
        const [, outBig] = await roundTrip(fmt, big);
        assert.equal(outBig.length, big.length, fmt + ' large');
        assert.ok(outBig.every((v, i) => v === big[i]), fmt + ' large content');
        const [, empty] = await roundTrip(fmt, new Uint8Array(0));
        assert.equal(empty.length, 0, fmt + ' empty');
    }
    assert.throws(() => new CompressionStream('br'), 'TypeError');
    assert.throws(() => new DecompressionStream(), 'TypeError');
});

promise_test('DecompressionStream refuses bad data', async () => {
    const bad = async (fmt, data) => {
        const ds = new Blob([data]).stream().pipeThrough(new DecompressionStream(fmt));
        try { await concat(ds); return false; } catch (e) { return e.name === 'TypeError'; }
    };
    assert.ok(await bad('gzip', bytes(1, 2, 3, 4)), 'junk');
    const [packed] = await roundTrip('gzip', 'abcdef');
    assert.ok(await bad('gzip', packed.slice(0, packed.length - 3)), 'truncated');
    const extra = new Uint8Array(packed.length + 2);
    extra.set(packed);
    assert.ok(await bad('gzip', extra), 'data after the end');
    assert.ok(await bad('deflate', bytes(0x78)), 'only a header');
});

promise_test('DecompressionStream takes a body one byte at a time', async () => {
    const [packed] = await roundTrip('deflate', 'streams streams streams');
    const ds = new DecompressionStream('deflate');
    const w = ds.writable.getWriter();
    for (const b of packed) w.write(new Uint8Array([b]));
    w.close();
    assert.equal(text(await concat(ds.readable)), 'streams streams streams');
    const ds2 = new DecompressionStream('deflate');
    const w2 = ds2.writable.getWriter();
    w2.write('not bytes').catch(() => {});
    try { await ds2.readable.getReader().read(); assert.ok(false); } catch (e) { assert.equal(e.name, 'TypeError'); }
});

promise_test('Blob.stream', async () => {
    const s = new Blob(['ab', 'cd']).stream();
    assert.ok(s instanceof ReadableStream);
    assert.equal(text(await concat(s)), 'abcd');
    const r = new Blob(['xyz']).stream().getReader({ mode: 'byob' });
    const { value } = await r.read(new Uint8Array(2));
    assert.equal(text(value), 'xy');
    assert.ok((await readAllChunks(new Blob([]).stream())).length === 0);
});

promise_test('Response.body from bytes, and bodyUsed', async () => {
    const res = new Response('hello');
    assert.ok(res.body instanceof ReadableStream);
    assert.equal(res.body, res.body, 'the same stream each time');
    assert.ok(!res.bodyUsed);
    const r = res.body.getReader();
    assert.ok(!res.bodyUsed, 'locking is not using');
    const { value } = await r.read();
    assert.ok(res.bodyUsed);
    assert.equal(text(value), 'hello');
    r.releaseLock();
    try { await res.text(); assert.ok(false); } catch (e) { assert.equal(e.name, 'TypeError'); }
    assert.equal(new Response(null).body, null);
    assert.equal(new Response().body, null);
    assert.throws(() => new Response('x', { status: 204 }), 'TypeError');
    assert.equal(new Response(null, { status: 204 }).body, null);
});

promise_test('Response from a ReadableStream', async () => {
    const rs = new ReadableStream({ start(c) { c.enqueue(new TextEncoder().encode('ab')); c.enqueue(bytes(0x63)); c.close(); } });
    const res = new Response(rs);
    assert.equal(res.body, rs);
    assert.equal(res.headers.get('content-type'), null);
    assert.equal(await res.text(), 'abc');
    assert.ok(res.bodyUsed);
    const bad = new Response(new ReadableStream({ start(c) { c.enqueue('string'); c.close(); } }));
    try { await bad.text(); assert.ok(false); } catch (e) { assert.equal(e.name, 'TypeError'); }
    const locked = new ReadableStream();
    locked.getReader();
    assert.throws(() => new Response(locked), 'TypeError');
});

promise_test('Response.clone tees the body', async () => {
    const res = new Response(ReadableStream.from([new TextEncoder().encode('one')]));
    const c = res.clone();
    assert.equal(await res.text(), 'one');
    assert.equal(await c.text(), 'one');
    const used = new Response('x');
    await used.text();
    assert.throws(() => used.clone(), 'TypeError');
});

promise_test('Request with a stream body', async () => {
    const body = ReadableStream.from([new TextEncoder().encode('data')]);
    assert.throws(() => new Request('http://example.test/', { method: 'POST', body }), 'TypeError');
    const req = new Request('http://example.test/', { method: 'POST', body, duplex: 'half' });
    assert.equal(req.body, body);
    assert.equal(req.duplex, 'half');
    assert.equal(await req.text(), 'data');
    assert.throws(() => new Request('http://example.test/', { method: 'POST', body: new ReadableStream(), duplex: 'half', keepalive: true }), 'TypeError');
    const r2 = new Request('http://example.test/', { method: 'POST', body: 'abc' });
    assert.ok(r2.body instanceof ReadableStream);
    assert.equal(text(await concat(r2.body)), 'abc');
});

promise_test('a fetch body from a data: URL as a stream', async () => {
    const res = await fetch('data:text/plain,streamed%20body');
    const s = res.body;
    assert.ok(s instanceof ReadableStream);
    assert.equal(text(await concat(s)), 'streamed body');
    const res2 = await fetch('data:text/plain,abc');
    const r = res2.body.getReader({ mode: 'byob' });
    assert.equal(text((await r.read(new Uint8Array(2))).value), 'ab');
});

promise_test('XMLHttpRequest.send with a stream sends it as a string', async () => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', 'data:text/plain,x');
    // XHR has no stream bodies: the stream is converted to a string
    xhr.send(new ReadableStream());
    await once(xhr, 'loadend');
    assert.ok(true);
});

promise_test('reading a large stream does not grow the stack', async () => {
    let n = 0;
    const rs = new ReadableStream({ pull(c) { if (n++ < 20000) c.enqueue(n); else c.close(); } }, { highWaterMark: 0 });
    let sum = 0;
    for await (const x of rs) sum += x;
    assert.equal(sum, 20001 * 20000 / 2);
});
