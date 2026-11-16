/* Aura's Streams: the WHATWG Streams Standard (ReadableStream with
 * default and byte controllers, default and BYOB readers, tee, pipeTo,
 * pipeThrough, async iteration and ReadableStream.from; WritableStream;
 * TransformStream; the two queuing strategies), the Encoding Standard's
 * TextEncoderStream and TextDecoderStream, and the Compression
 * Standard's CompressionStream and DecompressionStream over src/codec.
 *
 * The build compiles this file to its own bytecode (streams.bc); the
 * prelude reads it with N.streams() the first time script touches one of
 * these globals or a body's stream (Response.body, Blob.stream()), so a
 * page that never does pays only for the getters. It is a function of G,
 * the global object, and H, the prelude's helpers, and returns the
 * classes and the operations fetch needs (bodyFromFeed, readAll, ...).
 *
 * The abstract operations follow the standard's names and steps; each
 * object's internal slots are a record kept in a WeakMap, so page script
 * cannot reach or forge them. docs/script-requests-and-cors.md
 * ("Streams") says what is covered. */
(function (G, H) {
'use strict';

const N = H.N;
const domErr = H.domErr;
const methods = (proto, props) => {
    for (const k of Reflect.ownKeys(props)) {
        const d = Object.getOwnPropertyDescriptor(props, k);
        d.enumerable = true;
        d.configurable = true;
        if ('value' in d) d.writable = true;
        Object.defineProperty(proto, k, d);
    }
};
const tag = (C, name) => Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name || C.name, configurable: true });
const isObject = (v) => v !== null && (typeof v === 'object' || typeof v === 'function');
const S = new WeakMap(); /* object -> its internal slots */
const slots = (o, brand, what) => {
    const s = isObject(o) ? S.get(o) : undefined;
    if (!s || s.brand !== brand) throw new TypeError('Illegal invocation' + (what ? ': ' + what : ''));
    return s;
};
const is = (o, brand) => { const s = isObject(o) ? S.get(o) : undefined; return !!s && s.brand === brand; };
const noop = () => {};

/* ---------------- promises ---------------- */

const resolved = (v) => Promise.resolve(v);
const rejected = (e) => { const p = Promise.reject(e); p.catch(noop); return p; };
function deferred() {
    const d = {};
    d.promise = new Promise((a, b) => { d.resolve = a; d.reject = b; });
    d.state = 'pending';
    const ra = d.resolve, rb = d.reject;
    d.resolve = (v) => { if (d.state === 'pending') { d.state = 'fulfilled'; ra(v); } };
    d.reject = (e) => { if (d.state === 'pending') { d.state = 'rejected'; rb(e); } };
    return d;
}
const handled = (p) => { p.catch(noop); return p; };
const react = (p, onF, onR) => { const r = p.then(onF, onR); r.catch(noop); return r; };
/* "upon fulfillment" and "upon rejection" without creating an unhandled rejection */
const uponF = (p, f) => react(p, f, undefined);
const uponR = (p, f) => react(p, undefined, f);
/* Calls a method of a dictionary as a promise-returning callback. */
function promiseCall(fn, self, args) {
    if (fn === undefined) return resolved(undefined);
    try { return resolved(Reflect.apply(fn, self, args)); } catch (e) { return rejected(e); }
}
const microtask = (fn) => resolved().then(fn).catch((e) => H.reportError(e));

/* ---------------- WebIDL conversions ---------------- */

function dict(v, what) {
    if (v === undefined || v === null) return {};
    if (!isObject(v)) throw new TypeError(what + ' is not an object');
    return v;
}
function callback(v, what) {
    if (v === undefined) return undefined;
    if (typeof v !== 'function') throw new TypeError(what + ' is not a function');
    return v;
}
/* [EnforceRange] unsigned long long */
function enforceRange(v, what) {
    let x = Number(v);
    if (!Number.isFinite(x)) throw new TypeError(what + ' is not a finite number');
    x = Math.trunc(x);
    if (x < 0 || x > Number.MAX_SAFE_INTEGER) throw new TypeError(what + ' is out of range');
    return x;
}
const isView = (v) => ArrayBuffer.isView(v);
const isDetached = (b) => b.detached === true;
function transferBuffer(b) {
    if (isDetached(b)) throw new TypeError('The buffer is detached');
    return b.transfer();
}
const viewBuffer = (v) => v.buffer;
const elementSize = (v) => (v instanceof DataView ? 1 : v.BYTES_PER_ELEMENT);
const viewCtor = (v) => (v instanceof DataView ? DataView : v.constructor);

/* ---------------- queue with sizes ---------------- */

function resetQueue(c) { c.queue = []; c.queueTotalSize = 0; }
function enqueueValueWithSize(c, value, size) {
    size = Number(size);
    if (!(size >= 0) || size === Infinity) throw new RangeError('The chunk size is not a non-negative finite number');
    c.queue.push({ value, size });
    c.queueTotalSize += size;
}
function dequeueValue(c) {
    const pair = c.queue.shift();
    c.queueTotalSize -= pair.size;
    if (c.queueTotalSize < 0) c.queueTotalSize = 0;
    return pair.value;
}

/* ---------------- strategies ---------------- */

function extractHighWaterMark(strategy, def) {
    if (strategy.highWaterMark === undefined) return def;
    const h = Number(strategy.highWaterMark);
    if (Number.isNaN(h) || h < 0) throw new RangeError('Invalid highWaterMark');
    return h;
}
function extractSizeAlgorithm(strategy) {
    const size = strategy.size;
    if (size === undefined) return () => 1;
    return (chunk) => Number(Reflect.apply(size, undefined, [chunk]));
}
/* QueuingStrategy in member order: highWaterMark, size */
function strategyDict(v) {
    const d = dict(v, 'The strategy');
    const out = {};
    const hwm = d.highWaterMark;
    if (hwm !== undefined) out.highWaterMark = Number(hwm);
    out.size = callback(d.size, 'size');
    return out;
}

const byteLengthSize = { size(chunk) { return chunk.byteLength; } }.size;
const countSize = { size() { return 1; } }.size;
class ByteLengthQueuingStrategy {
    constructor(init) {
        if (!isObject(init)) throw new TypeError('ByteLengthQueuingStrategy: the argument is not an object');
        if (init.highWaterMark === undefined) throw new TypeError('ByteLengthQueuingStrategy: highWaterMark is required');
        S.set(this, { brand: 'BLQS', hwm: Number(init.highWaterMark) });
    }
}
methods(ByteLengthQueuingStrategy.prototype, {
    get highWaterMark() { return slots(this, 'BLQS').hwm; },
    get size() { slots(this, 'BLQS'); return byteLengthSize; },
});
tag(ByteLengthQueuingStrategy);
class CountQueuingStrategy {
    constructor(init) {
        if (!isObject(init)) throw new TypeError('CountQueuingStrategy: the argument is not an object');
        if (init.highWaterMark === undefined) throw new TypeError('CountQueuingStrategy: highWaterMark is required');
        S.set(this, { brand: 'CQS', hwm: Number(init.highWaterMark) });
    }
}
methods(CountQueuingStrategy.prototype, {
    get highWaterMark() { return slots(this, 'CQS').hwm; },
    get size() { slots(this, 'CQS'); return countSize; },
});
tag(CountQueuingStrategy);

/* ================= ReadableStream ================= */

class ReadableStream {
    constructor(underlyingSource = undefined, strategy = undefined) {
        /* WebIDL converts the strategy dictionary with the arguments,
         * before the body reads the source (an object, not a dictionary) */
        const st = strategyDict(strategy);
        const srcObj = underlyingSource === undefined ? null : underlyingSource;
        if (srcObj !== null && !isObject(srcObj)) throw new TypeError('ReadableStream: the source is not an object');
        const src = sourceDict(srcObj);
        const rs = initReadableStream(this);
        if (src.type === 'bytes') {
            if (st.size !== undefined) throw new RangeError('A byte stream cannot have a size function');
            setUpByteControllerFromSource(rs, srcObj, src, extractHighWaterMark(st, 0));
        } else {
            setUpDefaultControllerFromSource(rs, srcObj, src, extractHighWaterMark(st, 1), extractSizeAlgorithm(st));
        }
    }
    static from(asyncIterable) { return readableStreamFromIterable(asyncIterable); }
}
function sourceDict(o) {
    const d = {};
    if (o === null) return d;
    const aacs = o.autoAllocateChunkSize;
    if (aacs !== undefined) d.autoAllocateChunkSize = enforceRange(aacs, 'autoAllocateChunkSize');
    d.cancel = callback(o.cancel, 'cancel');
    d.pull = callback(o.pull, 'pull');
    d.start = callback(o.start, 'start');
    const type = o.type;
    if (type !== undefined) {
        d.type = String(type);
        if (d.type !== 'bytes') throw new TypeError('Invalid source type: ' + d.type);
    }
    return d;
}
function initReadableStream(obj) {
    const rs = { brand: 'RS', obj, state: 'readable', reader: undefined, storedError: undefined, disturbed: false, controller: undefined };
    S.set(obj, rs);
    return rs;
}
/* CreateReadableStream and CreateReadableByteStream */
function createReadableStream(start, pull, cancel, hwm = 1, size = () => 1) {
    const obj = Object.create(ReadableStream.prototype);
    const rs = initReadableStream(obj);
    const c = { brand: 'RSDC' };
    setUpDefaultController(rs, Object.create(ReadableStreamDefaultController.prototype), c, start, pull, cancel, hwm, size);
    return rs;
}
function createReadableByteStream(start, pull, cancel) {
    const obj = Object.create(ReadableStream.prototype);
    const rs = initReadableStream(obj);
    setUpByteController(rs, Object.create(ReadableByteStreamController.prototype), start, pull, cancel, 0, undefined);
    return rs;
}
const isLocked = (rs) => rs.reader !== undefined;

methods(ReadableStream.prototype, {
    get locked() { return isLocked(slots(this, 'RS')); },
    cancel(reason = undefined) {
        let rs;
        try { rs = slots(this, 'RS'); } catch (e) { return rejected(e); }
        if (isLocked(rs)) return rejected(new TypeError('Cannot cancel a locked stream'));
        return readableStreamCancel(rs, reason);
    },
    getReader(options = undefined) {
        const rs = slots(this, 'RS');
        const o = dict(options, 'The options');
        const mode = o.mode;
        if (mode === undefined) return acquireDefaultReader(rs).obj;
        if (String(mode) !== 'byob') throw new TypeError('Invalid reader mode: ' + mode);
        return acquireBYOBReader(rs).obj;
    },
    pipeThrough(transform, options = undefined) {
        const rs = slots(this, 'RS');
        if (!isObject(transform)) throw new TypeError('pipeThrough: the transform is not an object');
        const readable = transform.readable;
        const rr = is(readable, 'RS') ? S.get(readable) : null;
        if (!rr) throw new TypeError('pipeThrough: transform.readable is not a ReadableStream');
        const writable = transform.writable;
        const ww = is(writable, 'WS') ? S.get(writable) : null;
        if (!ww) throw new TypeError('pipeThrough: transform.writable is not a WritableStream');
        const o = pipeOptions(options);
        if (isLocked(rs)) throw new TypeError('pipeThrough: the stream is locked');
        if (isWritableLocked(ww)) throw new TypeError('pipeThrough: the writable stream is locked');
        handled(readableStreamPipeTo(rs, ww, o.preventClose, o.preventAbort, o.preventCancel, o.signal));
        return readable;
    },
    pipeTo(destination, options = undefined) {
        let rs, o;
        try {
            rs = slots(this, 'RS');
            if (!is(destination, 'WS')) throw new TypeError('pipeTo: the destination is not a WritableStream');
            o = pipeOptions(options);
        } catch (e) { return rejected(e); }
        const ws = S.get(destination);
        if (isLocked(rs)) return rejected(new TypeError('pipeTo: the stream is locked'));
        if (isWritableLocked(ws)) return rejected(new TypeError('pipeTo: the destination is locked'));
        return readableStreamPipeTo(rs, ws, o.preventClose, o.preventAbort, o.preventCancel, o.signal);
    },
    tee() {
        const rs = slots(this, 'RS');
        return readableStreamTee(rs, false).map((b) => b.obj);
    },
    values(options = undefined) {
        const rs = slots(this, 'RS');
        const o = dict(options, 'The options');
        return createAsyncIterator(rs, !!o.preventCancel);
    },
});
Object.defineProperty(ReadableStream.prototype, Symbol.asyncIterator, {
    value: ReadableStream.prototype.values, writable: true, configurable: true, enumerable: false,
});
tag(ReadableStream);
/* StreamPipeOptions in member order: preventAbort, preventCancel, preventClose, signal */
function pipeOptions(options) {
    const o = dict(options, 'The options');
    const r = { preventAbort: !!o.preventAbort, preventCancel: !!o.preventCancel, preventClose: !!o.preventClose, signal: o.signal };
    if (r.signal !== undefined && !(r.signal instanceof H.AbortSignal)) throw new TypeError('signal is not an AbortSignal');
    return r;
}

function readableStreamCancel(rs, reason) {
    rs.disturbed = true;
    if (rs.state === 'closed') return resolved(undefined);
    if (rs.state === 'errored') return rejected(rs.storedError);
    readableStreamClose(rs);
    const reader = rs.reader;
    if (reader !== undefined && reader.brand === 'BYOBR') {
        const list = reader.readIntoRequests;
        reader.readIntoRequests = [];
        for (const r of list) r.closeSteps(undefined);
    }
    const p = rs.controller.cancelSteps(reason);
    return react(p, () => undefined);
}
function readableStreamClose(rs) {
    rs.state = 'closed';
    const reader = rs.reader;
    if (reader === undefined) return;
    reader.closed.resolve(undefined);
    if (reader.brand === 'RSDR') {
        const list = reader.readRequests;
        reader.readRequests = [];
        for (const r of list) r.closeSteps();
    }
}
function readableStreamError(rs, e) {
    rs.state = 'errored';
    rs.storedError = e;
    const reader = rs.reader;
    if (reader === undefined) return;
    reader.closed.reject(e);
    handled(reader.closed.promise);
    if (reader.brand === 'RSDR') defaultReaderErrorReadRequests(reader, e);
    else byobReaderErrorReadIntoRequests(reader, e);
}
function addReadRequest(rs, req) { rs.reader.readRequests.push(req); }
function addReadIntoRequest(rs, req) { rs.reader.readIntoRequests.push(req); }
function fulfillReadRequest(rs, chunk, done) {
    const req = rs.reader.readRequests.shift();
    if (done) req.closeSteps(); else req.chunkSteps(chunk);
}
function fulfillReadIntoRequest(rs, chunk, done) {
    const req = rs.reader.readIntoRequests.shift();
    if (done) req.closeSteps(chunk); else req.chunkSteps(chunk);
}
const numReadRequests = (rs) => rs.reader.readRequests.length;
const numReadIntoRequests = (rs) => rs.reader.readIntoRequests.length;
const hasDefaultReader = (rs) => rs.reader !== undefined && rs.reader.brand === 'RSDR';
const hasBYOBReader = (rs) => rs.reader !== undefined && rs.reader.brand === 'BYOBR';

/* ---------------- readers ---------------- */

function genericReaderInit(reader, rs) {
    reader.stream = rs;
    rs.reader = reader;
    reader.closed = deferred();
    if (rs.state === 'closed') reader.closed.resolve(undefined);
    else if (rs.state === 'errored') { reader.closed.reject(rs.storedError); handled(reader.closed.promise); }
}
function genericReaderCancel(reader, reason) { return readableStreamCancel(reader.stream, reason); }
function genericReaderRelease(reader) {
    const rs = reader.stream;
    const e = new TypeError('The reader was released');
    if (rs.state === 'readable') reader.closed.reject(e);
    else { reader.closed = deferred(); reader.closed.reject(e); }
    handled(reader.closed.promise);
    rs.controller.releaseSteps();
    rs.reader = undefined;
    reader.stream = undefined;
}

class ReadableStreamDefaultReader {
    constructor(stream) {
        const rs = slots(stream, 'RS', 'the argument is not a ReadableStream');
        setUpDefaultReader(this, rs);
    }
}
function setUpDefaultReader(obj, rs) {
    if (isLocked(rs)) throw new TypeError('The stream is locked');
    const r = { brand: 'RSDR', obj, stream: undefined, closed: undefined, readRequests: [] };
    S.set(obj, r);
    genericReaderInit(r, rs);
    return r;
}
function acquireDefaultReader(rs) { return setUpDefaultReader(Object.create(ReadableStreamDefaultReader.prototype), rs); }
function defaultReaderRead(r, req) {
    const rs = r.stream;
    rs.disturbed = true;
    if (rs.state === 'closed') req.closeSteps();
    else if (rs.state === 'errored') req.errorSteps(rs.storedError);
    else rs.controller.pullSteps(req);
}
function defaultReaderRelease(r) {
    genericReaderRelease(r);
    defaultReaderErrorReadRequests(r, new TypeError('The reader was released'));
}
function defaultReaderErrorReadRequests(r, e) {
    const list = r.readRequests;
    r.readRequests = [];
    for (const req of list) req.errorSteps(e);
}
methods(ReadableStreamDefaultReader.prototype, {
    get closed() {
        let r;
        try { r = slots(this, 'RSDR'); } catch (e) { return rejected(e); }
        return r.closed.promise;
    },
    cancel(reason = undefined) {
        let r;
        try { r = slots(this, 'RSDR'); } catch (e) { return rejected(e); }
        if (r.stream === undefined) return rejected(new TypeError('The reader has no stream'));
        return genericReaderCancel(r, reason);
    },
    read() {
        let r;
        try { r = slots(this, 'RSDR'); } catch (e) { return rejected(e); }
        if (r.stream === undefined) return rejected(new TypeError('The reader has no stream'));
        const d = deferred();
        defaultReaderRead(r, {
            chunkSteps: (chunk) => d.resolve({ value: chunk, done: false }),
            closeSteps: () => d.resolve({ value: undefined, done: true }),
            errorSteps: (e) => d.reject(e),
        });
        return d.promise;
    },
    releaseLock() {
        const r = slots(this, 'RSDR');
        if (r.stream === undefined) return;
        defaultReaderRelease(r);
    },
});
tag(ReadableStreamDefaultReader);

class ReadableStreamBYOBReader {
    constructor(stream) {
        const rs = slots(stream, 'RS', 'the argument is not a ReadableStream');
        setUpBYOBReader(this, rs);
    }
}
function setUpBYOBReader(obj, rs) {
    if (isLocked(rs)) throw new TypeError('The stream is locked');
    if (rs.controller.brand !== 'RBSC') throw new TypeError('A BYOB reader needs a byte stream');
    const r = { brand: 'BYOBR', obj, stream: undefined, closed: undefined, readIntoRequests: [] };
    S.set(obj, r);
    genericReaderInit(r, rs);
    return r;
}
function acquireBYOBReader(rs) { return setUpBYOBReader(Object.create(ReadableStreamBYOBReader.prototype), rs); }
function byobReaderRead(r, view, min, req) {
    const rs = r.stream;
    rs.disturbed = true;
    if (rs.state === 'errored') req.errorSteps(rs.storedError);
    else byteControllerPullInto(rs.controller, view, min, req);
}
function byobReaderRelease(r) {
    genericReaderRelease(r);
    byobReaderErrorReadIntoRequests(r, new TypeError('The reader was released'));
}
function byobReaderErrorReadIntoRequests(r, e) {
    const list = r.readIntoRequests;
    r.readIntoRequests = [];
    for (const req of list) req.errorSteps(e);
}
methods(ReadableStreamBYOBReader.prototype, {
    get closed() {
        let r;
        try { r = slots(this, 'BYOBR'); } catch (e) { return rejected(e); }
        return r.closed.promise;
    },
    cancel(reason = undefined) {
        let r;
        try { r = slots(this, 'BYOBR'); } catch (e) { return rejected(e); }
        if (r.stream === undefined) return rejected(new TypeError('The reader has no stream'));
        return genericReaderCancel(r, reason);
    },
    read(view, options = undefined) {
        let r, min;
        try {
            r = slots(this, 'BYOBR');
            if (!isView(view)) throw new TypeError('read: the argument is not an ArrayBufferView');
            const o = dict(options, 'The options');
            min = o.min === undefined ? 1 : enforceRange(o.min, 'min');
        } catch (e) { return rejected(e); }
        if (view.byteLength === 0) return rejected(new TypeError('read: the view is empty'));
        if (viewBuffer(view).byteLength === 0) return rejected(new TypeError('read: the buffer is empty'));
        if (isDetached(viewBuffer(view))) return rejected(new TypeError('read: the buffer is detached'));
        if (min === 0) return rejected(new TypeError('read: min must be greater than 0'));
        if (view instanceof DataView) {
            if (min > view.byteLength) return rejected(new RangeError('read: min is larger than the view'));
        } else if (min > view.length) return rejected(new RangeError('read: min is larger than the view'));
        if (r.stream === undefined) return rejected(new TypeError('The reader has no stream'));
        const d = deferred();
        byobReaderRead(r, view, min, {
            chunkSteps: (chunk) => d.resolve({ value: chunk, done: false }),
            closeSteps: (chunk) => d.resolve({ value: chunk, done: true }),
            errorSteps: (e) => d.reject(e),
        });
        return d.promise;
    },
    releaseLock() {
        const r = slots(this, 'BYOBR');
        if (r.stream === undefined) return;
        byobReaderRelease(r);
    },
});
tag(ReadableStreamBYOBReader);

/* ---------------- ReadableStreamDefaultController ---------------- */

class ReadableStreamDefaultController {
    constructor() { throw new TypeError('Illegal constructor'); }
}
methods(ReadableStreamDefaultController.prototype, {
    get desiredSize() { return defaultControllerGetDesiredSize(slots(this, 'RSDC')); },
    close() {
        const c = slots(this, 'RSDC');
        if (!defaultControllerCanCloseOrEnqueue(c)) throw new TypeError('The stream cannot be closed');
        defaultControllerClose(c);
    },
    enqueue(chunk = undefined) {
        const c = slots(this, 'RSDC');
        if (!defaultControllerCanCloseOrEnqueue(c)) throw new TypeError('The stream cannot take more chunks');
        defaultControllerEnqueue(c, chunk);
    },
    error(e = undefined) { defaultControllerError(slots(this, 'RSDC'), e); },
});
tag(ReadableStreamDefaultController);

function setUpDefaultController(rs, obj, c, start, pull, cancel, hwm, size) {
    c.brand = 'RSDC';
    c.obj = obj;
    S.set(obj, c);
    c.stream = rs;
    resetQueue(c);
    c.started = c.closeRequested = c.pullAgain = c.pulling = false;
    c.strategySizeAlgorithm = size;
    c.strategyHWM = hwm;
    c.pullAlgorithm = pull;
    c.cancelAlgorithm = cancel;
    c.cancelSteps = (reason) => {
        resetQueue(c);
        const result = c.cancelAlgorithm(reason);
        defaultControllerClearAlgorithms(c);
        return result;
    };
    c.pullSteps = (req) => {
        if (c.queue.length > 0) {
            const chunk = dequeueValue(c);
            if (c.closeRequested && c.queue.length === 0) {
                defaultControllerClearAlgorithms(c);
                readableStreamClose(rs);
            } else defaultControllerCallPullIfNeeded(c);
            req.chunkSteps(chunk);
        } else {
            addReadRequest(rs, req);
            defaultControllerCallPullIfNeeded(c);
        }
    };
    c.releaseSteps = noop;
    rs.controller = c;
    const startResult = start();
    react(resolved(startResult), () => {
        c.started = true;
        defaultControllerCallPullIfNeeded(c);
    }, (r) => defaultControllerError(c, r));
}
function setUpDefaultControllerFromSource(rs, srcObj, src, hwm, size) {
    const obj = Object.create(ReadableStreamDefaultController.prototype);
    const c = {};
    setUpDefaultController(rs, obj, c,
        () => (src.start ? Reflect.apply(src.start, srcObj, [obj]) : undefined),
        () => promiseCall(src.pull, srcObj, [obj]),
        (reason) => promiseCall(src.cancel, srcObj, [reason]),
        hwm, size);
}
function defaultControllerCallPullIfNeeded(c) {
    if (!defaultControllerShouldCallPull(c)) return;
    if (c.pulling) { c.pullAgain = true; return; }
    c.pulling = true;
    react(c.pullAlgorithm(), () => {
        c.pulling = false;
        if (c.pullAgain) { c.pullAgain = false; defaultControllerCallPullIfNeeded(c); }
    }, (e) => defaultControllerError(c, e));
}
function defaultControllerShouldCallPull(c) {
    const rs = c.stream;
    if (!defaultControllerCanCloseOrEnqueue(c) || !c.started) return false;
    if (isLocked(rs) && hasDefaultReader(rs) && numReadRequests(rs) > 0) return true;
    return defaultControllerGetDesiredSize(c) > 0;
}
function defaultControllerClearAlgorithms(c) {
    c.pullAlgorithm = undefined;
    c.cancelAlgorithm = undefined;
    c.strategySizeAlgorithm = undefined;
}
function defaultControllerClose(c) {
    if (!defaultControllerCanCloseOrEnqueue(c)) return;
    c.closeRequested = true;
    if (c.queue.length === 0) {
        defaultControllerClearAlgorithms(c);
        readableStreamClose(c.stream);
    }
}
function defaultControllerEnqueue(c, chunk) {
    if (!defaultControllerCanCloseOrEnqueue(c)) return;
    const rs = c.stream;
    if (isLocked(rs) && hasDefaultReader(rs) && numReadRequests(rs) > 0) {
        fulfillReadRequest(rs, chunk, false);
    } else {
        let size;
        try { size = c.strategySizeAlgorithm(chunk); } catch (e) { defaultControllerError(c, e); throw e; }
        try { enqueueValueWithSize(c, chunk, size); } catch (e) { defaultControllerError(c, e); throw e; }
    }
    defaultControllerCallPullIfNeeded(c);
}
function defaultControllerError(c, e) {
    const rs = c.stream;
    if (rs.state !== 'readable') return;
    resetQueue(c);
    defaultControllerClearAlgorithms(c);
    readableStreamError(rs, e);
}
function defaultControllerGetDesiredSize(c) {
    const state = c.stream.state;
    if (state === 'errored') return null;
    if (state === 'closed') return 0;
    return c.strategyHWM - c.queueTotalSize;
}
const defaultControllerHasBackpressure = (c) => !defaultControllerShouldCallPull(c);
const defaultControllerCanCloseOrEnqueue = (c) => !c.closeRequested && c.stream.state === 'readable';

/* ---------------- ReadableByteStreamController ---------------- */

class ReadableByteStreamController {
    constructor() { throw new TypeError('Illegal constructor'); }
}
methods(ReadableByteStreamController.prototype, {
    get byobRequest() { return byteControllerGetBYOBRequest(slots(this, 'RBSC')); },
    get desiredSize() { return byteControllerGetDesiredSize(slots(this, 'RBSC')); },
    close() {
        const c = slots(this, 'RBSC');
        if (c.closeRequested) throw new TypeError('The stream is already closing');
        if (c.stream.state !== 'readable') throw new TypeError('The stream is not readable');
        byteControllerClose(c);
    },
    enqueue(chunk) {
        const c = slots(this, 'RBSC');
        if (!isView(chunk)) throw new TypeError('enqueue: the chunk is not an ArrayBufferView');
        if (chunk.byteLength === 0) throw new TypeError('enqueue: the chunk is empty');
        if (viewBuffer(chunk).byteLength === 0) throw new TypeError('enqueue: the chunk\'s buffer is empty');
        if (c.closeRequested) throw new TypeError('The stream is closing');
        if (c.stream.state !== 'readable') throw new TypeError('The stream is not readable');
        byteControllerEnqueue(c, chunk);
    },
    error(e = undefined) { byteControllerError(slots(this, 'RBSC'), e); },
});
tag(ReadableByteStreamController);

class ReadableStreamBYOBRequest {
    constructor() { throw new TypeError('Illegal constructor'); }
}
methods(ReadableStreamBYOBRequest.prototype, {
    get view() { return slots(this, 'BYOBQ').view; },
    respond(bytesWritten) {
        const q = slots(this, 'BYOBQ');
        bytesWritten = enforceRange(bytesWritten, 'bytesWritten');
        if (q.controller === undefined) throw new TypeError('The request has been answered');
        if (isDetached(viewBuffer(q.view))) throw new TypeError('The view\'s buffer is detached');
        byteControllerRespond(q.controller, bytesWritten);
    },
    respondWithNewView(view) {
        const q = slots(this, 'BYOBQ');
        if (!isView(view)) throw new TypeError('respondWithNewView: the argument is not an ArrayBufferView');
        if (q.controller === undefined) throw new TypeError('The request has been answered');
        if (isDetached(viewBuffer(view))) throw new TypeError('The view\'s buffer is detached');
        byteControllerRespondWithNewView(q.controller, view);
    },
});
tag(ReadableStreamBYOBRequest);

function setUpByteController(rs, obj, start, pull, cancel, hwm, aacs) {
    const c = { brand: 'RBSC', obj };
    S.set(obj, c);
    c.stream = rs;
    c.pullAgain = c.pulling = false;
    c.byobRequest = null;
    resetQueue(c);
    c.closeRequested = c.started = false;
    c.strategyHWM = hwm;
    c.pullAlgorithm = pull;
    c.cancelAlgorithm = cancel;
    c.autoAllocateChunkSize = aacs;
    c.pendingPullIntos = [];
    c.cancelSteps = (reason) => {
        byteControllerClearPendingPullIntos(c);
        resetQueue(c);
        const result = c.cancelAlgorithm(reason);
        byteControllerClearAlgorithms(c);
        return result;
    };
    c.pullSteps = (req) => {
        if (c.queueTotalSize > 0) { byteControllerFillReadRequestFromQueue(c, req); return; }
        const size = c.autoAllocateChunkSize;
        if (size !== undefined) {
            let buffer;
            try { buffer = new ArrayBuffer(size); } catch (e) { req.errorSteps(e); return; }
            c.pendingPullIntos.push({ buffer, bufferByteLength: size, byteOffset: 0, byteLength: size, bytesFilled: 0,
                minimumFill: 1, elementSize: 1, viewConstructor: Uint8Array, readerType: 'default' });
        }
        addReadRequest(rs, req);
        byteControllerCallPullIfNeeded(c);
    };
    c.releaseSteps = () => {
        if (c.pendingPullIntos.length) {
            const first = c.pendingPullIntos[0];
            first.readerType = 'none';
            c.pendingPullIntos = [first];
        }
    };
    rs.controller = c;
    const startResult = start();
    react(resolved(startResult), () => {
        c.started = true;
        byteControllerCallPullIfNeeded(c);
    }, (r) => byteControllerError(c, r));
    return c;
}
function setUpByteControllerFromSource(rs, srcObj, src, hwm) {
    const obj = Object.create(ReadableByteStreamController.prototype);
    if (src.autoAllocateChunkSize === 0) throw new TypeError('autoAllocateChunkSize must be greater than 0');
    setUpByteController(rs, obj,
        () => (src.start ? Reflect.apply(src.start, srcObj, [obj]) : undefined),
        () => promiseCall(src.pull, srcObj, [obj]),
        (reason) => promiseCall(src.cancel, srcObj, [reason]),
        hwm, src.autoAllocateChunkSize);
}
function byteControllerCallPullIfNeeded(c) {
    if (!byteControllerShouldCallPull(c)) return;
    if (c.pulling) { c.pullAgain = true; return; }
    c.pulling = true;
    react(c.pullAlgorithm(), () => {
        c.pulling = false;
        if (c.pullAgain) { c.pullAgain = false; byteControllerCallPullIfNeeded(c); }
    }, (e) => byteControllerError(c, e));
}
function byteControllerShouldCallPull(c) {
    const rs = c.stream;
    if (rs.state !== 'readable' || c.closeRequested || !c.started) return false;
    if (hasDefaultReader(rs) && numReadRequests(rs) > 0) return true;
    if (hasBYOBReader(rs) && numReadIntoRequests(rs) > 0) return true;
    return byteControllerGetDesiredSize(c) > 0;
}
function byteControllerClearAlgorithms(c) { c.pullAlgorithm = undefined; c.cancelAlgorithm = undefined; }
function byteControllerClearPendingPullIntos(c) {
    byteControllerInvalidateBYOBRequest(c);
    c.pendingPullIntos = [];
}
function byteControllerClose(c) {
    const rs = c.stream;
    if (c.closeRequested || rs.state !== 'readable') return;
    if (c.queueTotalSize > 0) { c.closeRequested = true; return; }
    if (c.pendingPullIntos.length) {
        const first = c.pendingPullIntos[0];
        if (first.bytesFilled % first.elementSize !== 0) {
            const e = new TypeError('The stream closed in the middle of an element');
            byteControllerError(c, e);
            throw e;
        }
    }
    byteControllerClearAlgorithms(c);
    readableStreamClose(rs);
}
function byteControllerCommitPullIntoDescriptor(rs, d) {
    let done = false;
    if (rs.state === 'closed') done = true;
    const filled = byteControllerConvertPullIntoDescriptor(d);
    if (d.readerType === 'default') fulfillReadRequest(rs, filled, done);
    else fulfillReadIntoRequest(rs, filled, done);
}
function byteControllerConvertPullIntoDescriptor(d) {
    const buffer = transferBuffer(d.buffer);
    return new d.viewConstructor(buffer, d.byteOffset, d.bytesFilled / d.elementSize);
}
function byteControllerEnqueue(c, chunk) {
    const rs = c.stream;
    if (c.closeRequested || rs.state !== 'readable') return;
    const buffer = viewBuffer(chunk), byteOffset = chunk.byteOffset, byteLength = chunk.byteLength;
    if (isDetached(buffer)) throw new TypeError('The chunk\'s buffer is detached');
    const transferred = transferBuffer(buffer);
    if (c.pendingPullIntos.length) {
        const first = c.pendingPullIntos[0];
        if (isDetached(first.buffer)) throw new TypeError('The pending read\'s buffer is detached');
        byteControllerInvalidateBYOBRequest(c);
        first.buffer = transferBuffer(first.buffer);
        if (first.readerType === 'none') byteControllerEnqueueDetachedPullIntoToQueue(c, first);
    }
    if (hasDefaultReader(rs)) {
        byteControllerProcessReadRequestsUsingQueue(c);
        if (numReadRequests(rs) === 0) {
            byteControllerEnqueueChunkToQueue(c, transferred, byteOffset, byteLength);
        } else {
            if (c.pendingPullIntos.length) byteControllerShiftPendingPullInto(c);
            fulfillReadRequest(rs, new Uint8Array(transferred, byteOffset, byteLength), false);
        }
    } else if (hasBYOBReader(rs)) {
        byteControllerEnqueueChunkToQueue(c, transferred, byteOffset, byteLength);
        for (const d of byteControllerProcessPullIntoDescriptorsUsingQueue(c)) byteControllerCommitPullIntoDescriptor(rs, d);
    } else {
        byteControllerEnqueueChunkToQueue(c, transferred, byteOffset, byteLength);
    }
    byteControllerCallPullIfNeeded(c);
}
function byteControllerEnqueueChunkToQueue(c, buffer, byteOffset, byteLength) {
    c.queue.push({ buffer, byteOffset, byteLength });
    c.queueTotalSize += byteLength;
}
function byteControllerEnqueueClonedChunkToQueue(c, buffer, byteOffset, byteLength) {
    let clone;
    try { clone = buffer.slice(byteOffset, byteOffset + byteLength); } catch (e) { byteControllerError(c, e); throw e; }
    byteControllerEnqueueChunkToQueue(c, clone, 0, byteLength);
}
function byteControllerEnqueueDetachedPullIntoToQueue(c, d) {
    if (d.bytesFilled > 0) byteControllerEnqueueClonedChunkToQueue(c, d.buffer, d.byteOffset, d.bytesFilled);
    byteControllerShiftPendingPullInto(c);
}
function byteControllerFillPullIntoDescriptorFromQueue(c, d) {
    const maxBytesToCopy = Math.min(c.queueTotalSize, d.byteLength - d.bytesFilled);
    const maxBytesFilled = d.bytesFilled + maxBytesToCopy;
    let remaining = maxBytesToCopy;
    let ready = false;
    const maxAlignedBytes = maxBytesFilled - (maxBytesFilled % d.elementSize);
    if (maxAlignedBytes >= d.minimumFill) {
        remaining = maxAlignedBytes - d.bytesFilled;
        ready = true;
    }
    const q = c.queue;
    while (remaining > 0) {
        const head = q[0];
        const n = Math.min(remaining, head.byteLength);
        const destStart = d.byteOffset + d.bytesFilled;
        new Uint8Array(d.buffer, destStart, n).set(new Uint8Array(head.buffer, head.byteOffset, n));
        if (head.byteLength === n) q.shift();
        else { head.byteOffset += n; head.byteLength -= n; }
        c.queueTotalSize -= n;
        byteControllerFillHeadPullIntoDescriptor(c, n, d);
        remaining -= n;
    }
    return ready;
}
function byteControllerFillHeadPullIntoDescriptor(c, size, d) { d.bytesFilled += size; }
function byteControllerFillReadRequestFromQueue(c, req) {
    const entry = c.queue.shift();
    c.queueTotalSize -= entry.byteLength;
    byteControllerHandleQueueDrain(c);
    req.chunkSteps(new Uint8Array(entry.buffer, entry.byteOffset, entry.byteLength));
}
function byteControllerGetBYOBRequest(c) {
    if (c.byobRequest === null && c.pendingPullIntos.length) {
        const first = c.pendingPullIntos[0];
        const view = new Uint8Array(first.buffer, first.byteOffset + first.bytesFilled, first.byteLength - first.bytesFilled);
        const obj = Object.create(ReadableStreamBYOBRequest.prototype);
        S.set(obj, { brand: 'BYOBQ', obj, controller: c, view });
        c.byobRequest = obj;
    }
    return c.byobRequest;
}
function byteControllerGetDesiredSize(c) {
    const state = c.stream.state;
    if (state === 'errored') return null;
    if (state === 'closed') return 0;
    return c.strategyHWM - c.queueTotalSize;
}
function byteControllerHandleQueueDrain(c) {
    if (c.queueTotalSize === 0 && c.closeRequested) {
        byteControllerClearAlgorithms(c);
        readableStreamClose(c.stream);
    } else byteControllerCallPullIfNeeded(c);
}
function byteControllerInvalidateBYOBRequest(c) {
    if (c.byobRequest === null) return;
    const q = S.get(c.byobRequest);
    q.controller = undefined;
    q.view = null;
    c.byobRequest = null;
}
function byteControllerProcessPullIntoDescriptorsUsingQueue(c) {
    const filled = [];
    while (c.pendingPullIntos.length) {
        if (c.queueTotalSize === 0) break;
        const d = c.pendingPullIntos[0];
        if (byteControllerFillPullIntoDescriptorFromQueue(c, d)) {
            byteControllerShiftPendingPullInto(c);
            filled.push(d);
        }
    }
    return filled;
}
function byteControllerProcessReadRequestsUsingQueue(c) {
    const reader = c.stream.reader;
    while (reader.readRequests.length) {
        if (c.queueTotalSize === 0) return;
        byteControllerFillReadRequestFromQueue(c, reader.readRequests.shift());
    }
}
function byteControllerPullInto(c, view, min, req) {
    const rs = c.stream;
    const esize = elementSize(view);
    const ctor = viewCtor(view);
    const minimumFill = min * esize;
    const byteOffset = view.byteOffset, byteLength = view.byteLength;
    let buffer;
    try { buffer = transferBuffer(viewBuffer(view)); } catch (e) { req.errorSteps(e); return; }
    const d = { buffer, bufferByteLength: buffer.byteLength, byteOffset, byteLength, bytesFilled: 0, minimumFill,
        elementSize: esize, viewConstructor: ctor, readerType: 'byob' };
    if (c.pendingPullIntos.length) {
        c.pendingPullIntos.push(d);
        addReadIntoRequest(rs, req);
        return;
    }
    if (rs.state === 'closed') {
        req.closeSteps(new ctor(d.buffer, d.byteOffset, 0));
        return;
    }
    if (c.queueTotalSize > 0) {
        if (byteControllerFillPullIntoDescriptorFromQueue(c, d)) {
            const filled = byteControllerConvertPullIntoDescriptor(d);
            byteControllerHandleQueueDrain(c);
            req.chunkSteps(filled);
            return;
        }
        if (c.closeRequested) {
            const e = new TypeError('The stream closed before the read could be filled');
            byteControllerError(c, e);
            req.errorSteps(e);
            return;
        }
    }
    c.pendingPullIntos.push(d);
    addReadIntoRequest(rs, req);
    byteControllerCallPullIfNeeded(c);
}
function byteControllerRespond(c, bytesWritten) {
    const first = c.pendingPullIntos[0];
    const state = c.stream.state;
    if (state === 'closed') {
        if (bytesWritten !== 0) throw new TypeError('respond: a closed stream takes 0 bytes');
    } else {
        if (bytesWritten === 0) throw new TypeError('respond: bytesWritten must be greater than 0');
        if (first.bytesFilled + bytesWritten > first.byteLength) throw new RangeError('respond: bytesWritten is too large');
    }
    first.buffer = transferBuffer(first.buffer);
    byteControllerRespondInternal(c, bytesWritten);
}
function byteControllerRespondInClosedState(c, first) {
    if (first.readerType === 'none') byteControllerShiftPendingPullInto(c);
    const rs = c.stream;
    if (hasBYOBReader(rs)) {
        const filled = [];
        while (filled.length < numReadIntoRequests(rs)) filled.push(byteControllerShiftPendingPullInto(c));
        for (const d of filled) byteControllerCommitPullIntoDescriptor(rs, d);
    }
}
function byteControllerRespondInReadableState(c, bytesWritten, d) {
    byteControllerFillHeadPullIntoDescriptor(c, bytesWritten, d);
    if (d.readerType === 'none') {
        byteControllerEnqueueDetachedPullIntoToQueue(c, d);
        for (const f of byteControllerProcessPullIntoDescriptorsUsingQueue(c)) byteControllerCommitPullIntoDescriptor(c.stream, f);
        return;
    }
    if (d.bytesFilled < d.minimumFill) return;
    byteControllerShiftPendingPullInto(c);
    const remainder = d.bytesFilled % d.elementSize;
    if (remainder > 0) {
        const end = d.byteOffset + d.bytesFilled;
        byteControllerEnqueueClonedChunkToQueue(c, d.buffer, end - remainder, remainder);
    }
    d.bytesFilled -= remainder;
    const filled = byteControllerProcessPullIntoDescriptorsUsingQueue(c);
    byteControllerCommitPullIntoDescriptor(c.stream, d);
    for (const f of filled) byteControllerCommitPullIntoDescriptor(c.stream, f);
}
function byteControllerRespondInternal(c, bytesWritten) {
    const first = c.pendingPullIntos[0];
    byteControllerInvalidateBYOBRequest(c);
    if (c.stream.state === 'closed') byteControllerRespondInClosedState(c, first);
    else byteControllerRespondInReadableState(c, bytesWritten, first);
    byteControllerCallPullIfNeeded(c);
}
function byteControllerRespondWithNewView(c, view) {
    const first = c.pendingPullIntos[0];
    const state = c.stream.state;
    if (state === 'closed') {
        if (view.byteLength !== 0) throw new TypeError('respondWithNewView: a closed stream takes an empty view');
    } else if (view.byteLength === 0) throw new TypeError('respondWithNewView: the view is empty');
    if (first.byteOffset + first.bytesFilled !== view.byteOffset) throw new RangeError('respondWithNewView: the view starts at the wrong place');
    if (first.bufferByteLength !== viewBuffer(view).byteLength) throw new RangeError('respondWithNewView: the view\'s buffer has the wrong length');
    if (first.bytesFilled + view.byteLength > first.byteLength) throw new RangeError('respondWithNewView: the view is too long');
    const n = view.byteLength;
    first.buffer = transferBuffer(viewBuffer(view));
    byteControllerRespondInternal(c, n);
}
function byteControllerShiftPendingPullInto(c) { return c.pendingPullIntos.shift(); }
function byteControllerError(c, e) {
    const rs = c.stream;
    if (rs.state !== 'readable') return;
    byteControllerClearPendingPullIntos(c);
    resetQueue(c);
    byteControllerClearAlgorithms(c);
    readableStreamError(rs, e);
}

/* ---------------- tee ---------------- */

function readableStreamTee(rs, cloneForBranch2) {
    if (rs.controller.brand === 'RBSC') return readableByteStreamTee(rs);
    return readableStreamDefaultTee(rs, cloneForBranch2);
}
function readableStreamDefaultTee(rs, cloneForBranch2) {
    const reader = acquireDefaultReader(rs);
    let reading = false, readAgain = false, canceled1 = false, canceled2 = false, reason1, reason2, branch1, branch2;
    const cancelP = deferred();
    const pull = () => {
        if (reading) { readAgain = true; return resolved(undefined); }
        reading = true;
        defaultReaderRead(reader, {
            chunkSteps: (chunk) => microtask(() => {
                readAgain = false;
                const chunk1 = chunk;
                let chunk2 = chunk;
                if (!canceled2 && cloneForBranch2) {
                    try { chunk2 = H.structuredClone(chunk2); } catch (e) {
                        defaultControllerError(branch1.controller, e);
                        defaultControllerError(branch2.controller, e);
                        cancelP.resolve(readableStreamCancel(rs, e));
                        return;
                    }
                }
                if (!canceled1) defaultControllerEnqueue(branch1.controller, chunk1);
                if (!canceled2) defaultControllerEnqueue(branch2.controller, chunk2);
                reading = false;
                if (readAgain) pull();
            }),
            closeSteps: () => {
                reading = false;
                if (!canceled1) defaultControllerClose(branch1.controller);
                if (!canceled2) defaultControllerClose(branch2.controller);
                if (!canceled1 || !canceled2) cancelP.resolve(undefined);
            },
            errorSteps: () => { reading = false; },
        });
        return resolved(undefined);
    };
    const cancel1 = (reason) => {
        canceled1 = true;
        reason1 = reason;
        if (canceled2) cancelP.resolve(readableStreamCancel(rs, [reason1, reason2]));
        return cancelP.promise;
    };
    const cancel2 = (reason) => {
        canceled2 = true;
        reason2 = reason;
        if (canceled1) cancelP.resolve(readableStreamCancel(rs, [reason1, reason2]));
        return cancelP.promise;
    };
    branch1 = createReadableStream(noop, pull, cancel1);
    branch2 = createReadableStream(noop, pull, cancel2);
    uponR(reader.closed.promise, (r) => {
        defaultControllerError(branch1.controller, r);
        defaultControllerError(branch2.controller, r);
        if (!canceled1 || !canceled2) cancelP.resolve(undefined);
    });
    return [branch1, branch2];
}
function cloneAsUint8Array(o) {
    return new Uint8Array(viewBuffer(o).slice(o.byteOffset, o.byteOffset + o.byteLength));
}
function readableByteStreamTee(rs) {
    let reader = acquireDefaultReader(rs);
    let reading = false, readAgain1 = false, readAgain2 = false, canceled1 = false, canceled2 = false;
    let reason1, reason2, branch1, branch2;
    const cancelP = deferred();
    const forwardReaderError = (thisReader) => {
        uponR(thisReader.closed.promise, (r) => {
            if (thisReader !== reader) return;
            byteControllerError(branch1.controller, r);
            byteControllerError(branch2.controller, r);
            if (!canceled1 || !canceled2) cancelP.resolve(undefined);
        });
    };
    const pullWithDefaultReader = () => {
        if (reader.brand === 'BYOBR') {
            byobReaderRelease(reader);
            reader = acquireDefaultReader(rs);
            forwardReaderError(reader);
        }
        defaultReaderRead(reader, {
            chunkSteps: (chunk) => microtask(() => {
                readAgain1 = readAgain2 = false;
                const chunk1 = chunk;
                let chunk2 = chunk;
                if (!canceled1 && !canceled2) {
                    try { chunk2 = cloneAsUint8Array(chunk); } catch (e) {
                        byteControllerError(branch1.controller, e);
                        byteControllerError(branch2.controller, e);
                        cancelP.resolve(readableStreamCancel(rs, e));
                        return;
                    }
                }
                if (!canceled1) byteControllerEnqueue(branch1.controller, chunk1);
                if (!canceled2) byteControllerEnqueue(branch2.controller, chunk2);
                reading = false;
                if (readAgain1) pull1(); else if (readAgain2) pull2();
            }),
            closeSteps: () => {
                reading = false;
                if (!canceled1) byteControllerClose(branch1.controller);
                if (!canceled2) byteControllerClose(branch2.controller);
                if (branch1.controller.pendingPullIntos.length) byteControllerRespond(branch1.controller, 0);
                if (branch2.controller.pendingPullIntos.length) byteControllerRespond(branch2.controller, 0);
                if (!canceled1 || !canceled2) cancelP.resolve(undefined);
            },
            errorSteps: () => { reading = false; },
        });
    };
    const pullWithBYOBReader = (view, forBranch2) => {
        if (reader.brand === 'RSDR') {
            defaultReaderRelease(reader);
            reader = acquireBYOBReader(rs);
            forwardReaderError(reader);
        }
        const byobBranch = forBranch2 ? branch2 : branch1;
        const otherBranch = forBranch2 ? branch1 : branch2;
        byobReaderRead(reader, view, 1, {
            chunkSteps: (chunk) => microtask(() => {
                readAgain1 = readAgain2 = false;
                const byobCanceled = forBranch2 ? canceled2 : canceled1;
                const otherCanceled = forBranch2 ? canceled1 : canceled2;
                if (!otherCanceled) {
                    let clone;
                    try { clone = cloneAsUint8Array(chunk); } catch (e) {
                        byteControllerError(byobBranch.controller, e);
                        byteControllerError(otherBranch.controller, e);
                        cancelP.resolve(readableStreamCancel(rs, e));
                        return;
                    }
                    if (!byobCanceled) byteControllerRespondWithNewView(byobBranch.controller, chunk);
                    byteControllerEnqueue(otherBranch.controller, clone);
                } else if (!byobCanceled) byteControllerRespondWithNewView(byobBranch.controller, chunk);
                reading = false;
                if (readAgain1) pull1(); else if (readAgain2) pull2();
            }),
            closeSteps: (chunk) => {
                reading = false;
                const byobCanceled = forBranch2 ? canceled2 : canceled1;
                const otherCanceled = forBranch2 ? canceled1 : canceled2;
                if (!byobCanceled) byteControllerClose(byobBranch.controller);
                if (!otherCanceled) byteControllerClose(otherBranch.controller);
                if (chunk !== undefined) {
                    if (!byobCanceled) byteControllerRespondWithNewView(byobBranch.controller, chunk);
                    if (!otherCanceled && otherBranch.controller.pendingPullIntos.length) byteControllerRespond(otherBranch.controller, 0);
                }
                if (!byobCanceled || !otherCanceled) cancelP.resolve(undefined);
            },
            errorSteps: () => { reading = false; },
        });
    };
    const pull1 = () => {
        if (reading) { readAgain1 = true; return resolved(undefined); }
        reading = true;
        const q = byteControllerGetBYOBRequest(branch1.controller);
        if (q === null) pullWithDefaultReader(); else pullWithBYOBReader(S.get(q).view, false);
        return resolved(undefined);
    };
    const pull2 = () => {
        if (reading) { readAgain2 = true; return resolved(undefined); }
        reading = true;
        const q = byteControllerGetBYOBRequest(branch2.controller);
        if (q === null) pullWithDefaultReader(); else pullWithBYOBReader(S.get(q).view, true);
        return resolved(undefined);
    };
    const cancel1 = (reason) => {
        canceled1 = true;
        reason1 = reason;
        if (canceled2) cancelP.resolve(readableStreamCancel(rs, [reason1, reason2]));
        return cancelP.promise;
    };
    const cancel2 = (reason) => {
        canceled2 = true;
        reason2 = reason;
        if (canceled1) cancelP.resolve(readableStreamCancel(rs, [reason1, reason2]));
        return cancelP.promise;
    };
    branch1 = createReadableByteStream(noop, pull1, cancel1);
    branch2 = createReadableByteStream(noop, pull2, cancel2);
    forwardReaderError(reader);
    return [branch1, branch2];
}

/* ---------------- pipeTo ---------------- */

function readableStreamPipeTo(source, dest, preventClose, preventAbort, preventCancel, signal) {
    const reader = acquireDefaultReader(source);
    const writer = acquireWriter(dest);
    source.disturbed = true;
    let shuttingDown = false;
    let currentWrite = resolved(undefined);
    const done = deferred();
    let abortAlgorithm;
    const waitForWritesToFinish = () => {
        const old = currentWrite;
        return react(currentWrite, () => (old !== currentWrite ? waitForWritesToFinish() : undefined));
    };
    const finalize = (isError, error) => {
        writerRelease(writer);
        defaultReaderRelease(reader);
        if (signal !== undefined) signal.removeEventListener('abort', abortAlgorithm);
        if (isError) done.reject(error); else done.resolve(undefined);
    };
    const shutdownWithAnAction = (action, originalIsError, originalError) => {
        if (shuttingDown) return;
        shuttingDown = true;
        const doTheRest = () => {
            react(action(), () => finalize(originalIsError, originalError), (e) => finalize(true, e));
        };
        if (dest.state === 'writable' && !writableStreamCloseQueuedOrInFlight(dest)) uponF(waitForWritesToFinish(), doTheRest);
        else doTheRest();
    };
    const shutdown = (isError, error) => {
        if (shuttingDown) return;
        shuttingDown = true;
        if (dest.state === 'writable' && !writableStreamCloseQueuedOrInFlight(dest))
            uponF(waitForWritesToFinish(), () => finalize(isError, error));
        else finalize(isError, error);
    };
    if (signal !== undefined) {
        abortAlgorithm = () => {
            const error = signal.reason;
            const actions = [];
            if (!preventAbort) actions.push(() => (dest.state === 'writable' ? writableStreamAbort(dest, error) : resolved(undefined)));
            if (!preventCancel) actions.push(() => (source.state === 'readable' ? readableStreamCancel(source, error) : resolved(undefined)));
            shutdownWithAnAction(() => Promise.all(actions.map((a) => a())), true, error);
        };
        if (signal.aborted) {
            abortAlgorithm();
            return done.promise;
        }
        signal.addEventListener('abort', abortAlgorithm);
    }
    const pipeStep = () => {
        if (shuttingDown) return resolved(true);
        return writer.ready.promise.then(() => new Promise((resolveRead, rejectRead) => {
            defaultReaderRead(reader, {
                chunkSteps: (chunk) => {
                    currentWrite = react(writerWrite(writer, chunk), undefined, noop);
                    resolveRead(false);
                },
                closeSteps: () => resolveRead(true),
                errorSteps: rejectRead,
            });
        }));
    };
    const pipeLoop = () => new Promise((resolveLoop, rejectLoop) => {
        const next = (stop) => { if (stop) resolveLoop(); else pipeStep().then(next, rejectLoop); };
        next(false);
    });
    /* errors must propagate forward */
    if (source.state === 'errored') {
        const e = source.storedError;
        if (!preventAbort) shutdownWithAnAction(() => writableStreamAbort(dest, e), true, e); else shutdown(true, e);
    } else uponR(reader.closed.promise, (e) => {
        if (!preventAbort) shutdownWithAnAction(() => writableStreamAbort(dest, e), true, e); else shutdown(true, e);
    });
    /* errors must propagate backward */
    if (dest.state === 'errored') {
        const e = dest.storedError;
        if (!preventCancel) shutdownWithAnAction(() => readableStreamCancel(source, e), true, e); else shutdown(true, e);
    } else uponR(writer.closed.promise, (e) => {
        if (!preventCancel) shutdownWithAnAction(() => readableStreamCancel(source, e), true, e); else shutdown(true, e);
    });
    /* closing must propagate forward */
    if (source.state === 'closed') {
        if (!preventClose) shutdownWithAnAction(() => writerCloseWithErrorPropagation(writer)); else shutdown();
    } else uponF(reader.closed.promise, () => {
        if (!preventClose) shutdownWithAnAction(() => writerCloseWithErrorPropagation(writer)); else shutdown();
    });
    /* closing must propagate backward */
    if (writableStreamCloseQueuedOrInFlight(dest) || dest.state === 'closed') {
        const e = new TypeError('The destination closed before the source was piped to it');
        if (!preventCancel) shutdownWithAnAction(() => readableStreamCancel(source, e), true, e); else shutdown(true, e);
    }
    handled(pipeLoop());
    return done.promise;
}

/* ---------------- async iteration, from() ---------------- */

const AsyncIteratorPrototype = Object.getPrototypeOf(Object.getPrototypeOf(async function* () {}).prototype);
const ReadableStreamAsyncIteratorPrototype = Object.create(AsyncIteratorPrototype);
function createAsyncIterator(rs, preventCancel) {
    const reader = acquireDefaultReader(rs);
    const it = Object.create(ReadableStreamAsyncIteratorPrototype);
    S.set(it, { brand: 'RSAI', reader, preventCancel, ongoing: null, finished: false });
    return it;
}
methods(ReadableStreamAsyncIteratorPrototype, {
    next() {
        let s;
        try { s = slots(this, 'RSAI'); } catch (e) { return rejected(e); }
        const nextSteps = () => {
            if (s.finished) return resolved({ value: undefined, done: true });
            const reader = s.reader;
            if (reader.stream === undefined) return rejected(new TypeError('The iterator has released its reader'));
            const d = deferred();
            defaultReaderRead(reader, {
                chunkSteps: (chunk) => d.resolve({ value: chunk, done: false }),
                closeSteps: () => { defaultReaderRelease(reader); d.resolve({ value: undefined, done: true }); },
                errorSteps: (e) => { defaultReaderRelease(reader); d.reject(e); },
            });
            return d.promise.then((r) => {
                s.ongoing = null;
                if (r.done) s.finished = true;
                return r;
            }, (e) => {
                s.ongoing = null;
                s.finished = true;
                throw e;
            });
        };
        s.ongoing = s.ongoing ? s.ongoing.then(nextSteps, nextSteps) : nextSteps();
        return s.ongoing;
    },
    return(value = undefined) {
        let s;
        try { s = slots(this, 'RSAI'); } catch (e) { return rejected(e); }
        const returnSteps = () => {
            if (s.finished) return resolved({ value, done: true });
            s.finished = true;
            const reader = s.reader;
            if (reader.stream === undefined) return rejected(new TypeError('The iterator has released its reader'));
            if (!s.preventCancel) {
                const result = genericReaderCancel(reader, value);
                defaultReaderRelease(reader);
                return result;
            }
            defaultReaderRelease(reader);
            return resolved(undefined);
        };
        s.ongoing = s.ongoing ? s.ongoing.then(returnSteps, returnSteps) : returnSteps();
        return s.ongoing.then(() => ({ value, done: true }));
    },
});
Object.defineProperty(ReadableStreamAsyncIteratorPrototype, Symbol.toStringTag, {
    value: 'ReadableStream AsyncIterator', configurable: true,
});

function readableStreamFromIterable(asyncIterable) {
    let iterator;
    if (asyncIterable === null || asyncIterable === undefined) throw new TypeError('ReadableStream.from: the argument is not iterable');
    const method = asyncIterable[Symbol.asyncIterator];
    if (method !== undefined && method !== null) {
        if (typeof method !== 'function') throw new TypeError('ReadableStream.from: Symbol.asyncIterator is not a function');
        iterator = Reflect.apply(method, asyncIterable, []);
    } else {
        const sync = asyncIterable[Symbol.iterator];
        if (typeof sync !== 'function') throw new TypeError('ReadableStream.from: the argument is not iterable');
        const syncIt = Reflect.apply(sync, asyncIterable, []);
        if (!isObject(syncIt)) throw new TypeError('ReadableStream.from: the iterator is not an object');
        iterator = (async function* () { yield* { [Symbol.iterator]: () => syncIt }; })();
    }
    if (!isObject(iterator)) throw new TypeError('ReadableStream.from: the iterator is not an object');
    const nextMethod = iterator.next;
    let rs;
    const pull = () => {
        let nextResult;
        try { nextResult = Reflect.apply(nextMethod, iterator, []); } catch (e) { return rejected(e); }
        return resolved(nextResult).then((r) => {
            if (!isObject(r)) throw new TypeError('ReadableStream.from: the iterator result is not an object');
            if (r.done) defaultControllerClose(rs.controller);
            else defaultControllerEnqueue(rs.controller, r.value);
        });
    };
    const cancel = (reason) => {
        let ret;
        try {
            ret = iterator.return;
            if (ret === undefined || ret === null) return resolved(undefined);
            if (typeof ret !== 'function') throw new TypeError('ReadableStream.from: return is not a function');
        } catch (e) { return rejected(e); }
        let r;
        try { r = Reflect.apply(ret, iterator, [reason]); } catch (e) { return rejected(e); }
        return resolved(r).then((x) => {
            if (!isObject(x)) throw new TypeError('ReadableStream.from: the return result is not an object');
        });
    };
    rs = createReadableStream(noop, pull, cancel, 0);
    return rs.obj;
}

/* ================= WritableStream ================= */

class WritableStream {
    constructor(underlyingSink = undefined, strategy = undefined) {
        const st = strategyDict(strategy);
        const sinkObj = underlyingSink === undefined ? null : underlyingSink;
        if (sinkObj !== null && !isObject(sinkObj)) throw new TypeError('WritableStream: the sink is not an object');
        const sink = sinkDict(sinkObj);
        if (sink.type !== undefined) throw new RangeError('Invalid sink type');
        const ws = initWritableStream(this);
        const obj = Object.create(WritableStreamDefaultController.prototype);
        setUpWritableController(ws, obj,
            () => (sink.start ? Reflect.apply(sink.start, sinkObj, [obj]) : undefined),
            (chunk) => promiseCall(sink.write, sinkObj, [chunk, obj]),
            () => promiseCall(sink.close, sinkObj, []),
            (reason) => promiseCall(sink.abort, sinkObj, [reason]),
            extractHighWaterMark(st, 1), extractSizeAlgorithm(st));
    }
}
function sinkDict(o) {
    const d = {};
    if (o === null) return d;
    d.abort = callback(o.abort, 'abort');
    d.close = callback(o.close, 'close');
    d.start = callback(o.start, 'start');
    d.type = o.type;
    d.write = callback(o.write, 'write');
    return d;
}
function initWritableStream(obj) {
    const ws = { brand: 'WS', obj, state: 'writable', storedError: undefined, writer: undefined, controller: undefined,
        inFlightWriteRequest: undefined, closeRequest: undefined, inFlightCloseRequest: undefined,
        pendingAbortRequest: undefined, writeRequests: [], backpressure: false };
    S.set(obj, ws);
    return ws;
}
function createWritableStream(start, write, close, abort, hwm, size) {
    const obj = Object.create(WritableStream.prototype);
    const ws = initWritableStream(obj);
    setUpWritableController(ws, Object.create(WritableStreamDefaultController.prototype), start, write, close, abort, hwm, size);
    return ws;
}
const isWritableLocked = (ws) => ws.writer !== undefined;
methods(WritableStream.prototype, {
    get locked() { return isWritableLocked(slots(this, 'WS')); },
    abort(reason = undefined) {
        let ws;
        try { ws = slots(this, 'WS'); } catch (e) { return rejected(e); }
        if (isWritableLocked(ws)) return rejected(new TypeError('Cannot abort a locked stream'));
        return writableStreamAbort(ws, reason);
    },
    close() {
        let ws;
        try { ws = slots(this, 'WS'); } catch (e) { return rejected(e); }
        if (isWritableLocked(ws)) return rejected(new TypeError('Cannot close a locked stream'));
        if (writableStreamCloseQueuedOrInFlight(ws)) return rejected(new TypeError('The stream is already closing'));
        return writableStreamClose(ws);
    },
    getWriter() { return acquireWriter(slots(this, 'WS')).obj; },
});
tag(WritableStream);

function writableStreamAbort(ws, reason) {
    if (ws.state === 'closed' || ws.state === 'errored') return resolved(undefined);
    ws.controller.abortController.abort(reason);
    const state = ws.state;
    if (state === 'closed' || state === 'errored') return resolved(undefined);
    if (ws.pendingAbortRequest !== undefined) return ws.pendingAbortRequest.d.promise;
    let wasAlreadyErroring = false;
    if (state === 'erroring') { wasAlreadyErroring = true; reason = undefined; }
    const d = deferred();
    ws.pendingAbortRequest = { d, reason, wasAlreadyErroring };
    if (!wasAlreadyErroring) writableStreamStartErroring(ws, reason);
    return d.promise;
}
function writableStreamClose(ws) {
    const state = ws.state;
    if (state === 'closed' || state === 'errored') return rejected(new TypeError('The stream is closed or errored'));
    const d = deferred();
    ws.closeRequest = d;
    const writer = ws.writer;
    if (writer !== undefined && ws.backpressure && state === 'writable') writer.ready.resolve(undefined);
    writableControllerClose(ws.controller);
    return d.promise;
}
function writableStreamAddWriteRequest(ws) {
    const d = deferred();
    ws.writeRequests.push(d);
    return d.promise;
}
function writableStreamDealWithRejection(ws, error) {
    if (ws.state === 'writable') { writableStreamStartErroring(ws, error); return; }
    writableStreamFinishErroring(ws);
}
function writableStreamFinishErroring(ws) {
    ws.state = 'errored';
    ws.controller.errorSteps();
    const storedError = ws.storedError;
    for (const d of ws.writeRequests) d.reject(storedError);
    ws.writeRequests = [];
    if (ws.pendingAbortRequest === undefined) { writableStreamRejectCloseAndClosedPromiseIfNeeded(ws); return; }
    const abortRequest = ws.pendingAbortRequest;
    ws.pendingAbortRequest = undefined;
    if (abortRequest.wasAlreadyErroring) {
        abortRequest.d.reject(storedError);
        writableStreamRejectCloseAndClosedPromiseIfNeeded(ws);
        return;
    }
    react(ws.controller.abortSteps(abortRequest.reason), () => {
        abortRequest.d.resolve(undefined);
        writableStreamRejectCloseAndClosedPromiseIfNeeded(ws);
    }, (reason) => {
        abortRequest.d.reject(reason);
        writableStreamRejectCloseAndClosedPromiseIfNeeded(ws);
    });
}
function writableStreamFinishInFlightClose(ws) {
    ws.inFlightCloseRequest.resolve(undefined);
    ws.inFlightCloseRequest = undefined;
    if (ws.state === 'erroring') {
        ws.storedError = undefined;
        if (ws.pendingAbortRequest !== undefined) {
            ws.pendingAbortRequest.d.resolve(undefined);
            ws.pendingAbortRequest = undefined;
        }
    }
    ws.state = 'closed';
    if (ws.writer !== undefined) ws.writer.closed.resolve(undefined);
}
function writableStreamFinishInFlightCloseWithError(ws, error) {
    ws.inFlightCloseRequest.reject(error);
    ws.inFlightCloseRequest = undefined;
    if (ws.pendingAbortRequest !== undefined) {
        ws.pendingAbortRequest.d.reject(error);
        ws.pendingAbortRequest = undefined;
    }
    writableStreamDealWithRejection(ws, error);
}
function writableStreamFinishInFlightWrite(ws) {
    ws.inFlightWriteRequest.resolve(undefined);
    ws.inFlightWriteRequest = undefined;
}
function writableStreamFinishInFlightWriteWithError(ws, error) {
    ws.inFlightWriteRequest.reject(error);
    ws.inFlightWriteRequest = undefined;
    writableStreamDealWithRejection(ws, error);
}
const writableStreamHasOperationMarkedInFlight = (ws) => ws.inFlightWriteRequest !== undefined || ws.inFlightCloseRequest !== undefined;
function writableStreamMarkCloseRequestInFlight(ws) {
    ws.inFlightCloseRequest = ws.closeRequest;
    ws.closeRequest = undefined;
}
function writableStreamMarkFirstWriteRequestInFlight(ws) { ws.inFlightWriteRequest = ws.writeRequests.shift(); }
function writableStreamRejectCloseAndClosedPromiseIfNeeded(ws) {
    if (ws.closeRequest !== undefined) {
        ws.closeRequest.reject(ws.storedError);
        ws.closeRequest = undefined;
    }
    const writer = ws.writer;
    if (writer !== undefined) {
        writer.closed.reject(ws.storedError);
        handled(writer.closed.promise);
    }
}
function writableStreamStartErroring(ws, reason) {
    const c = ws.controller;
    ws.state = 'erroring';
    ws.storedError = reason;
    if (ws.writer !== undefined) writerEnsureReadyPromiseRejected(ws.writer, reason);
    if (!writableStreamHasOperationMarkedInFlight(ws) && c.started) writableStreamFinishErroring(ws);
}
function writableStreamUpdateBackpressure(ws, backpressure) {
    const writer = ws.writer;
    if (writer !== undefined && backpressure !== ws.backpressure) {
        if (backpressure) writer.ready = deferred();
        else writer.ready.resolve(undefined);
    }
    ws.backpressure = backpressure;
}
const writableStreamCloseQueuedOrInFlight = (ws) => ws.closeRequest !== undefined || ws.inFlightCloseRequest !== undefined;

/* ---------------- WritableStreamDefaultWriter ---------------- */

class WritableStreamDefaultWriter {
    constructor(stream) {
        const ws = slots(stream, 'WS', 'the argument is not a WritableStream');
        setUpWriter(this, ws);
    }
}
function setUpWriter(obj, ws) {
    if (isWritableLocked(ws)) throw new TypeError('The stream is locked');
    const w = { brand: 'WSDW', obj, stream: ws, ready: deferred(), closed: deferred() };
    S.set(obj, w);
    ws.writer = w;
    const state = ws.state;
    if (state === 'writable') {
        if (!writableStreamCloseQueuedOrInFlight(ws) && ws.backpressure) { /* ready stays pending */ }
        else w.ready.resolve(undefined);
    } else if (state === 'erroring') {
        w.ready.reject(ws.storedError);
        handled(w.ready.promise);
    } else if (state === 'closed') {
        w.ready.resolve(undefined);
        w.closed.resolve(undefined);
    } else {
        w.ready.reject(ws.storedError);
        handled(w.ready.promise);
        w.closed.reject(ws.storedError);
        handled(w.closed.promise);
    }
    return w;
}
function acquireWriter(ws) { return setUpWriter(Object.create(WritableStreamDefaultWriter.prototype), ws); }
function writerCloseWithErrorPropagation(w) {
    const ws = w.stream;
    const state = ws.state;
    if (writableStreamCloseQueuedOrInFlight(ws) || state === 'closed') return resolved(undefined);
    if (state === 'errored') return rejected(ws.storedError);
    return writableStreamClose(ws);
}
function writerEnsureClosedPromiseRejected(w, error) {
    if (w.closed.state !== 'pending') w.closed = deferred();
    w.closed.reject(error);
    handled(w.closed.promise);
}
function writerEnsureReadyPromiseRejected(w, error) {
    if (w.ready.state !== 'pending') w.ready = deferred();
    w.ready.reject(error);
    handled(w.ready.promise);
}
function writerGetDesiredSize(w) {
    const ws = w.stream;
    const state = ws.state;
    if (state === 'errored' || state === 'erroring') return null;
    if (state === 'closed') return 0;
    return writableControllerGetDesiredSize(ws.controller);
}
function writerRelease(w) {
    const ws = w.stream;
    const e = new TypeError('The writer was released');
    writerEnsureReadyPromiseRejected(w, e);
    writerEnsureClosedPromiseRejected(w, e);
    ws.writer = undefined;
    w.stream = undefined;
}
function writerWrite(w, chunk) {
    const ws = w.stream;
    const c = ws.controller;
    const size = writableControllerGetChunkSize(c, chunk);
    if (ws !== w.stream) return rejected(new TypeError('The writer was released'));
    const state = ws.state;
    if (state === 'errored') return rejected(ws.storedError);
    if (writableStreamCloseQueuedOrInFlight(ws) || state === 'closed') return rejected(new TypeError('The stream is closing or closed'));
    if (state === 'erroring') return rejected(ws.storedError);
    const p = writableStreamAddWriteRequest(ws);
    writableControllerWrite(c, chunk, size);
    return p;
}
methods(WritableStreamDefaultWriter.prototype, {
    get closed() {
        let w;
        try { w = slots(this, 'WSDW'); } catch (e) { return rejected(e); }
        return w.closed.promise;
    },
    get desiredSize() {
        const w = slots(this, 'WSDW');
        if (w.stream === undefined) throw new TypeError('The writer has been released');
        return writerGetDesiredSize(w);
    },
    get ready() {
        let w;
        try { w = slots(this, 'WSDW'); } catch (e) { return rejected(e); }
        return w.ready.promise;
    },
    abort(reason = undefined) {
        let w;
        try { w = slots(this, 'WSDW'); } catch (e) { return rejected(e); }
        if (w.stream === undefined) return rejected(new TypeError('The writer has been released'));
        return writableStreamAbort(w.stream, reason);
    },
    close() {
        let w;
        try { w = slots(this, 'WSDW'); } catch (e) { return rejected(e); }
        const ws = w.stream;
        if (ws === undefined) return rejected(new TypeError('The writer has been released'));
        if (writableStreamCloseQueuedOrInFlight(ws)) return rejected(new TypeError('The stream is already closing'));
        return writableStreamClose(ws);
    },
    releaseLock() {
        const w = slots(this, 'WSDW');
        if (w.stream === undefined) return;
        writerRelease(w);
    },
    write(chunk = undefined) {
        let w;
        try { w = slots(this, 'WSDW'); } catch (e) { return rejected(e); }
        if (w.stream === undefined) return rejected(new TypeError('The writer has been released'));
        return writerWrite(w, chunk);
    },
});
tag(WritableStreamDefaultWriter);

/* ---------------- WritableStreamDefaultController ---------------- */

const closeSentinel = {};
class WritableStreamDefaultController {
    constructor() { throw new TypeError('Illegal constructor'); }
}
methods(WritableStreamDefaultController.prototype, {
    get signal() { return slots(this, 'WSDC').abortController.signal; },
    error(e = undefined) {
        const c = slots(this, 'WSDC');
        if (c.stream.state !== 'writable') return;
        writableControllerError(c, e);
    },
});
tag(WritableStreamDefaultController);

function setUpWritableController(ws, obj, start, write, close, abort, hwm, size) {
    const c = { brand: 'WSDC', obj };
    S.set(obj, c);
    c.stream = ws;
    ws.controller = c;
    resetQueue(c);
    c.abortController = new H.AbortController();
    c.started = false;
    c.strategySizeAlgorithm = size;
    c.strategyHWM = hwm;
    c.writeAlgorithm = write;
    c.closeAlgorithm = close;
    c.abortAlgorithm = abort;
    c.abortSteps = (reason) => {
        const result = c.abortAlgorithm(reason);
        writableControllerClearAlgorithms(c);
        return result;
    };
    c.errorSteps = () => resetQueue(c);
    writableStreamUpdateBackpressure(ws, writableControllerGetBackpressure(c));
    const startResult = start();
    react(resolved(startResult), () => {
        c.started = true;
        writableControllerAdvanceQueueIfNeeded(c);
    }, (r) => {
        c.started = true;
        writableStreamDealWithRejection(ws, r);
    });
}
function writableControllerAdvanceQueueIfNeeded(c) {
    const ws = c.stream;
    if (!c.started || ws.inFlightWriteRequest !== undefined) return;
    if (ws.state === 'erroring') { writableStreamFinishErroring(ws); return; }
    if (c.queue.length === 0) return;
    const value = c.queue[0].value;
    if (value === closeSentinel) writableControllerProcessClose(c);
    else writableControllerProcessWrite(c, value);
}
function writableControllerClearAlgorithms(c) {
    c.writeAlgorithm = c.closeAlgorithm = c.abortAlgorithm = c.strategySizeAlgorithm = undefined;
}
function writableControllerClose(c) {
    enqueueValueWithSize(c, closeSentinel, 0);
    writableControllerAdvanceQueueIfNeeded(c);
}
function writableControllerGetChunkSize(c, chunk) {
    if (c.strategySizeAlgorithm === undefined) return 1;
    try { return c.strategySizeAlgorithm(chunk); } catch (e) { writableControllerErrorIfNeeded(c, e); return 1; }
}
const writableControllerGetDesiredSize = (c) => c.strategyHWM - c.queueTotalSize;
function writableControllerErrorIfNeeded(c, e) { if (c.stream.state === 'writable') writableControllerError(c, e); }
function writableControllerProcessClose(c) {
    const ws = c.stream;
    writableStreamMarkCloseRequestInFlight(ws);
    dequeueValue(c);
    const p = c.closeAlgorithm();
    writableControllerClearAlgorithms(c);
    react(p, () => writableStreamFinishInFlightClose(ws), (r) => writableStreamFinishInFlightCloseWithError(ws, r));
}
function writableControllerProcessWrite(c, chunk) {
    const ws = c.stream;
    writableStreamMarkFirstWriteRequestInFlight(ws);
    react(c.writeAlgorithm(chunk), () => {
        writableStreamFinishInFlightWrite(ws);
        const state = ws.state;
        dequeueValue(c);
        if (!writableStreamCloseQueuedOrInFlight(ws) && state === 'writable')
            writableStreamUpdateBackpressure(ws, writableControllerGetBackpressure(c));
        writableControllerAdvanceQueueIfNeeded(c);
    }, (reason) => {
        if (ws.state === 'writable') writableControllerClearAlgorithms(c);
        writableStreamFinishInFlightWriteWithError(ws, reason);
    });
}
function writableControllerWrite(c, chunk, size) {
    try { enqueueValueWithSize(c, chunk, size); } catch (e) { writableControllerErrorIfNeeded(c, e); return; }
    const ws = c.stream;
    if (!writableStreamCloseQueuedOrInFlight(ws) && ws.state === 'writable')
        writableStreamUpdateBackpressure(ws, writableControllerGetBackpressure(c));
    writableControllerAdvanceQueueIfNeeded(c);
}
const writableControllerGetBackpressure = (c) => writableControllerGetDesiredSize(c) <= 0;
function writableControllerError(c, e) {
    writableControllerClearAlgorithms(c);
    writableStreamStartErroring(c.stream, e);
}

/* ================= TransformStream ================= */

class TransformStream {
    constructor(transformer = undefined, writableStrategy = undefined, readableStrategy = undefined) {
        const ws = strategyDict(writableStrategy);
        const rsS = strategyDict(readableStrategy);
        const tObj = transformer === undefined ? null : transformer;
        if (tObj !== null && !isObject(tObj)) throw new TypeError('TransformStream: the transformer is not an object');
        const t = transformerDict(tObj);
        if (t.readableType !== undefined) throw new RangeError('Invalid readableType');
        if (t.writableType !== undefined) throw new RangeError('Invalid writableType');
        const start = deferred();
        const ts = initTransformStream(this, start, extractHighWaterMark(ws, 1), extractSizeAlgorithm(ws),
            extractHighWaterMark(rsS, 0), extractSizeAlgorithm(rsS));
        setUpTransformControllerFromTransformer(ts, tObj, t);
        if (t.start) start.resolve(Reflect.apply(t.start, tObj, [ts.controller.obj]));
        else start.resolve(undefined);
    }
}
function transformerDict(o) {
    const d = {};
    if (o === null) return d;
    d.cancel = callback(o.cancel, 'cancel');
    d.flush = callback(o.flush, 'flush');
    d.readableType = o.readableType;
    d.start = callback(o.start, 'start');
    d.transform = callback(o.transform, 'transform');
    d.writableType = o.writableType;
    return d;
}
function initTransformStream(obj, start, wHWM, wSize, rHWM, rSize) {
    const ts = { brand: 'TS', obj, backpressure: undefined, backpressureChange: undefined, controller: undefined };
    S.set(obj, ts);
    const startAlgorithm = () => start.promise;
    ts.writable = createWritableStream(startAlgorithm, (chunk) => transformSinkWrite(ts, chunk),
        () => transformSinkClose(ts), (reason) => transformSinkAbort(ts, reason), wHWM, wSize);
    ts.readable = createReadableStream(startAlgorithm, () => transformSourcePull(ts),
        (reason) => transformSourceCancel(ts, reason), rHWM, rSize);
    transformStreamSetBackpressure(ts, true);
    return ts;
}
methods(TransformStream.prototype, {
    get readable() { return slots(this, 'TS').readable.obj; },
    get writable() { return slots(this, 'TS').writable.obj; },
});
tag(TransformStream);
function transformStreamError(ts, e) {
    defaultControllerError(ts.readable.controller, e);
    transformStreamErrorWritableAndUnblockWrite(ts, e);
}
function transformStreamErrorWritableAndUnblockWrite(ts, e) {
    transformControllerClearAlgorithms(ts.controller);
    writableControllerErrorIfNeeded(ts.writable.controller, e);
    transformStreamUnblockWrite(ts);
}
function transformStreamUnblockWrite(ts) { if (ts.backpressure) transformStreamSetBackpressure(ts, false); }
function transformStreamSetBackpressure(ts, bp) {
    if (ts.backpressureChange !== undefined) ts.backpressureChange.resolve(undefined);
    ts.backpressureChange = deferred();
    ts.backpressure = bp;
}

class TransformStreamDefaultController {
    constructor() { throw new TypeError('Illegal constructor'); }
}
methods(TransformStreamDefaultController.prototype, {
    get desiredSize() { return defaultControllerGetDesiredSize(slots(this, 'TSDC').stream.readable.controller); },
    enqueue(chunk = undefined) { transformControllerEnqueue(slots(this, 'TSDC'), chunk); },
    error(reason = undefined) { transformStreamError(slots(this, 'TSDC').stream, reason); },
    terminate() { transformControllerTerminate(slots(this, 'TSDC')); },
});
tag(TransformStreamDefaultController);

function setUpTransformController(ts, obj, transform, flush, cancel) {
    const c = { brand: 'TSDC', obj, stream: ts, transformAlgorithm: transform, flushAlgorithm: flush,
        cancelAlgorithm: cancel, finish: undefined };
    S.set(obj, c);
    ts.controller = c;
    return c;
}
function setUpTransformControllerFromTransformer(ts, tObj, t) {
    const obj = Object.create(TransformStreamDefaultController.prototype);
    let transform = (chunk) => {
        try { transformControllerEnqueue(S.get(obj), chunk); return resolved(undefined); } catch (e) { return rejected(e); }
    };
    if (t.transform) transform = (chunk) => promiseCall(t.transform, tObj, [chunk, obj]);
    const flush = t.flush ? () => promiseCall(t.flush, tObj, [obj]) : () => resolved(undefined);
    const cancel = t.cancel ? (reason) => promiseCall(t.cancel, tObj, [reason]) : () => resolved(undefined);
    setUpTransformController(ts, obj, transform, flush, cancel);
}
function transformControllerClearAlgorithms(c) {
    c.transformAlgorithm = c.flushAlgorithm = c.cancelAlgorithm = undefined;
}
function transformControllerEnqueue(c, chunk) {
    const ts = c.stream;
    const rc = ts.readable.controller;
    if (!defaultControllerCanCloseOrEnqueue(rc)) throw new TypeError('The readable side cannot take more chunks');
    try { defaultControllerEnqueue(rc, chunk); } catch (e) {
        transformStreamErrorWritableAndUnblockWrite(ts, e);
        throw ts.readable.storedError;
    }
    const bp = defaultControllerHasBackpressure(rc);
    if (bp !== ts.backpressure) transformStreamSetBackpressure(ts, true);
}
function transformControllerPerformTransform(c, chunk) {
    return react(c.transformAlgorithm(chunk), undefined, (r) => {
        transformStreamError(c.stream, r);
        throw r;
    });
}
function transformControllerTerminate(c) {
    const ts = c.stream;
    defaultControllerClose(ts.readable.controller);
    transformStreamErrorWritableAndUnblockWrite(ts, new TypeError('The transform stream was terminated'));
}
function transformSinkWrite(ts, chunk) {
    const c = ts.controller;
    if (ts.backpressure) {
        return react(ts.backpressureChange.promise, () => {
            const w = ts.writable;
            if (w.state === 'erroring') throw w.storedError;
            return transformControllerPerformTransform(c, chunk);
        });
    }
    return transformControllerPerformTransform(c, chunk);
}
function transformSinkAbort(ts, reason) {
    const c = ts.controller;
    if (c.finish !== undefined) return c.finish.promise;
    const readable = ts.readable;
    c.finish = deferred();
    const p = c.cancelAlgorithm(reason);
    transformControllerClearAlgorithms(c);
    react(p, () => {
        if (readable.state === 'errored') c.finish.reject(readable.storedError);
        else { defaultControllerError(readable.controller, reason); c.finish.resolve(undefined); }
    }, (r) => {
        defaultControllerError(readable.controller, r);
        c.finish.reject(r);
    });
    return c.finish.promise;
}
function transformSinkClose(ts) {
    const c = ts.controller;
    if (c.finish !== undefined) return c.finish.promise;
    const readable = ts.readable;
    c.finish = deferred();
    const p = c.flushAlgorithm();
    transformControllerClearAlgorithms(c);
    react(p, () => {
        if (readable.state === 'errored') c.finish.reject(readable.storedError);
        else { defaultControllerClose(readable.controller); c.finish.resolve(undefined); }
    }, (r) => {
        defaultControllerError(readable.controller, r);
        c.finish.reject(r);
    });
    return c.finish.promise;
}
function transformSourceCancel(ts, reason) {
    const c = ts.controller;
    if (c.finish !== undefined) return c.finish.promise;
    const writable = ts.writable;
    c.finish = deferred();
    const p = c.cancelAlgorithm(reason);
    transformControllerClearAlgorithms(c);
    react(p, () => {
        if (writable.state === 'errored') c.finish.reject(writable.storedError);
        else {
            writableControllerErrorIfNeeded(writable.controller, reason);
            transformStreamUnblockWrite(ts);
            c.finish.resolve(undefined);
        }
    }, (r) => {
        writableControllerErrorIfNeeded(writable.controller, r);
        transformStreamUnblockWrite(ts);
        c.finish.reject(r);
    });
    return c.finish.promise;
}
function transformSourcePull(ts) {
    transformStreamSetBackpressure(ts, false);
    return ts.backpressureChange.promise;
}

/* A TransformStream made here from algorithms (the encoding and
 * compression streams): its readable and writable, and its controller's
 * slots. */
function makeTransform(transform, flush, cancel) {
    const obj = Object.create(TransformStream.prototype);
    const start = deferred();
    const ts = initTransformStream(obj, start, 1, () => 1, 0, () => 1);
    const cobj = Object.create(TransformStreamDefaultController.prototype);
    const c = setUpTransformController(ts, cobj, null, null, null);
    c.transformAlgorithm = (chunk) => { try { transform(c, chunk); return resolved(undefined); } catch (e) { return rejected(e); } };
    c.flushAlgorithm = () => { try { flush(c); return resolved(undefined); } catch (e) { return rejected(e); } };
    c.cancelAlgorithm = () => { try { cancel(); } catch (e) { /* nothing to report */ } return resolved(undefined); };
    start.resolve(undefined);
    return ts;
}

/* ================= TextEncoderStream, TextDecoderStream ================= */

class TextEncoderStream {
    constructor() {
        let pending = '';
        const ts = makeTransform((c, chunk) => {
            let s = pending + String(chunk);
            pending = '';
            const last = s.charCodeAt(s.length - 1);
            if (last >= 0xd800 && last <= 0xdbff) { pending = s.slice(-1); s = s.slice(0, -1); }
            if (s.length) {
                const b = N.utf8Encode(s.toWellFormed());
                if (b.length) transformControllerEnqueue(c, b);
            }
        }, (c) => {
            if (pending) transformControllerEnqueue(c, new Uint8Array([0xef, 0xbf, 0xbd]));
        }, noop);
        S.set(this, { brand: 'TES', ts });
    }
}
methods(TextEncoderStream.prototype, {
    get encoding() { slots(this, 'TES'); return 'utf-8'; },
    get readable() { return slots(this, 'TES').ts.readable.obj; },
    get writable() { return slots(this, 'TES').ts.writable.obj; },
});
tag(TextEncoderStream);

function bufferSourceView(chunk, what) {
    if (chunk instanceof ArrayBuffer) return new Uint8Array(chunk);
    if (isView(chunk)) return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    throw new TypeError(what + ': the chunk is not an ArrayBuffer or ArrayBufferView');
}
class TextDecoderStream {
    constructor(label = 'utf-8', options = undefined) {
        const dec = new H.TextDecoder(label, options);
        const ts = makeTransform((c, chunk) => {
            const s = dec.decode(bufferSourceView(chunk, 'TextDecoderStream'), { stream: true });
            if (s.length) transformControllerEnqueue(c, s);
        }, (c) => {
            const s = dec.decode();
            if (s.length) transformControllerEnqueue(c, s);
        }, noop);
        S.set(this, { brand: 'TDS', ts, dec });
    }
}
methods(TextDecoderStream.prototype, {
    get encoding() { return slots(this, 'TDS').dec.encoding; },
    get fatal() { return slots(this, 'TDS').dec.fatal; },
    get ignoreBOM() { return slots(this, 'TDS').dec.ignoreBOM; },
    get readable() { return slots(this, 'TDS').ts.readable.obj; },
    get writable() { return slots(this, 'TDS').ts.writable.obj; },
});
tag(TextDecoderStream);

/* ================= CompressionStream, DecompressionStream ================= */

const zFormats = { gzip: 0, deflate: 1, 'deflate-raw': 2 };
function zipTransform(decompress, format, what) {
    format = String(format);
    if (!(format in zFormats)) throw new TypeError(what + ': unsupported format ' + format);
    let h = null;
    const handle = () => h || (h = N.zNew(decompress, zFormats[format]));
    const end = () => { if (h) { N.zEnd(h); h = null; } };
    const run = (c, bytes, finish) => {
        let r;
        try { r = N.zRun(handle(), bytes, finish); } catch (e) { end(); throw e; }
        for (;;) {
            if (r[0].length) transformControllerEnqueue(c, r[0]);
            if (!r[1]) break;
            r = N.zRun(h, null, finish);
        }
    };
    /* The codec is made with the first chunk, so a stream that is never
     * written to holds none (an encoder is 300 KB). */
    return makeTransform((c, chunk) => {
        const v = bufferSourceView(chunk, what);
        run(c, v.length ? v.slice() : null, false);
    }, (c) => {
        if (!h && decompress) { handle(); }
        run(c, null, true);
        end();
    }, end);
}
class CompressionStream {
    constructor(format) {
        if (!arguments.length) throw new TypeError('CompressionStream: 1 argument required');
        S.set(this, { brand: 'CS', ts: zipTransform(false, format, 'CompressionStream') });
    }
}
methods(CompressionStream.prototype, {
    get readable() { return slots(this, 'CS').ts.readable.obj; },
    get writable() { return slots(this, 'CS').ts.writable.obj; },
});
tag(CompressionStream);
class DecompressionStream {
    constructor(format) {
        if (!arguments.length) throw new TypeError('DecompressionStream: 1 argument required');
        S.set(this, { brand: 'DS', ts: zipTransform(true, format, 'DecompressionStream') });
    }
}
methods(DecompressionStream.prototype, {
    get readable() { return slots(this, 'DS').ts.readable.obj; },
    get writable() { return slots(this, 'DS').ts.writable.obj; },
});
tag(DecompressionStream);

/* ================= for fetch, Blob and the prelude ================= */

const CHUNK = 65536;
/* A byte stream over bytes held in memory, given out in pieces as they
 * are read (each a copy: a reader may transfer its buffer). */
function streamFromBytes(bytes) {
    let off = 0;
    let rs;
    const closeNow = (c) => {
        byteControllerClose(c);
        if (rs.state === 'closed' && c.pendingPullIntos.length) byteControllerRespond(c, 0);
    };
    rs = createReadableByteStream(noop, () => {
        const c = rs.controller;
        if (c.closeRequested || rs.state !== 'readable') return resolved(undefined);
        if (off < bytes.length) {
            const n = Math.min(CHUNK, bytes.length - off);
            byteControllerEnqueue(c, bytes.slice(off, off + n));
            off += n;
        }
        if (off >= bytes.length) closeNow(c);
        return resolved(undefined);
    }, () => { bytes = new Uint8Array(0); return resolved(undefined); });
    if (!bytes.length) closeNow(rs.controller);
    return rs.obj;
}
/* A byte stream fed from a fetch's network body (prelude_net.js, "A
 * network body as it arrives"): the feed's sink gets what came so far and
 * every later piece. The source pauses the network while any of its
 * streams holds PAUSE_AT bytes its reader has not taken, and pulling
 * resumes it. */
const PAUSE_AT = 65536;
const feedWants = (f) => !f.sink || f.sink.wanted();
function sourceUpdate(src) {
    if (src.ended || !src.pause) return;
    const want = src.feeds.every(feedWants);
    if (want === src.paused) { src.paused = !want; src.pause(src.paused); }
}
function sourceDrop(src, f) {
    src.feeds = src.feeds.filter((x) => x !== f);
    if (!src.feeds.length && !src.ended && src.abort) { src.ended = true; src.abort(); }
    else sourceUpdate(src);
}
function bodyFromFeed(feed) {
    const src = feed.src;
    let rs;
    const sink = {
        chunk(bytes) {
            const c = rs.controller;
            if (c.closeRequested || rs.state !== 'readable') return;
            byteControllerEnqueue(c, bytes);
        },
        close() {
            const c = rs.controller;
            if (c.closeRequested || rs.state !== 'readable') return;
            byteControllerClose(c);
            if (c.pendingPullIntos.length && rs.state === 'closed') byteControllerRespond(c, 0);
        },
        error(e) { byteControllerError(rs.controller, e); },
        wanted() { return rs.state === 'readable' && rs.controller.queueTotalSize < PAUSE_AT; },
    };
    rs = createReadableByteStream(noop, () => { sourceUpdate(src); return resolved(undefined); }, () => {
        sourceDrop(src, feed);
        return resolved(undefined);
    });
    feed.sink = sink;
    src.update = sourceUpdate;
    for (const c of feed.chunks) sink.chunk(c);
    feed.chunks = [];
    if (feed.state === 1) sink.close();
    else if (feed.state === 2) sink.error(feed.err);
    sourceUpdate(src);
    return rs.obj;
}
/* Reads a stream of Uint8Array chunks to the end: a promise of one
 * Uint8Array (fetch's "fully read"). */
function readAll(stream) {
    const rs = slots(stream, 'RS');
    const reader = acquireDefaultReader(rs);
    const parts = [];
    let total = 0;
    const d = deferred();
    const step = () => {
        defaultReaderRead(reader, {
            chunkSteps: (chunk) => {
                if (!(chunk instanceof Uint8Array)) {
                    const e = new TypeError('The body stream gave a chunk that is not a Uint8Array');
                    d.reject(e);
                    readableStreamCancel(rs, e);
                    return;
                }
                parts.push(chunk);
                total += chunk.length;
                microtask(step);
            },
            closeSteps: () => {
                const out = new Uint8Array(total);
                let o = 0;
                for (const p of parts) { out.set(p, o); o += p.length; }
                d.resolve(out);
            },
            errorSteps: (e) => d.reject(e),
        });
    };
    step();
    return d.promise;
}
/* A stream that is already read (a body consumed before .body was asked for). */
function usedStream() {
    const rs = createReadableByteStream(noop, () => resolved(undefined), () => resolved(undefined));
    byteControllerClose(rs.controller);
    acquireDefaultReader(rs);
    rs.disturbed = true;
    return rs.obj;
}
const isReadable = (x) => is(x, 'RS');
const streamState = (x) => { const rs = S.get(x); return { locked: isLocked(rs), disturbed: rs.disturbed }; };
const tee = (x) => readableStreamTee(S.get(x), true).map((b) => b.obj);

return {
    ReadableStream, ReadableStreamDefaultReader, ReadableStreamBYOBReader, ReadableStreamDefaultController,
    ReadableByteStreamController, ReadableStreamBYOBRequest, WritableStream, WritableStreamDefaultWriter,
    WritableStreamDefaultController, TransformStream, TransformStreamDefaultController,
    ByteLengthQueuingStrategy, CountQueuingStrategy, TextEncoderStream, TextDecoderStream,
    CompressionStream, DecompressionStream,
    streamFromBytes, bodyFromFeed, readAll, usedStream, isReadable, streamState, tee,
};
})
