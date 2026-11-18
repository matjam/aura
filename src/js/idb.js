/* Aura's IndexedDB (W3C Indexed Database API 3.0). The build compiles
 * this file to its own bytecode (idb.bc); the prelude reads it with
 * N.lazy(3) the first time script touches indexedDB or one of the IDB
 * classes (lazyModule in prelude.js). It is a function of G and H, the
 * prelude's helpers, and returns the globals.
 *
 * The databases live in memory, per document or worker, for as long as
 * it lives: a page that stores state finds the API working and its data
 * there until it goes; a reload starts empty. docs/javascript.md
 * ("IndexedDB") has the details.
 *
 * Storage: a store keeps its records in an array sorted by key
 * (binary search to find, splice to insert); an index keeps
 * [indexKey, primaryKey] pairs sorted by both. Values are stored as
 * structured clones and cloned again when read.
 *
 * Transactions run their requests in order, one per task, and commit
 * when none is left and script has had its turn (the task after the
 * last request's event). A transaction that writes saves a copy of each
 * store's arrays (and, for an upgrade, the database's shape) the first
 * time it touches them, and puts them back if it aborts. */
(function (G, H) {
'use strict';

const N = H.N;
const domErr = H.domErr;

/* ---------------- Keys ---------------- */

const T_NUM = 1, T_DATE = 2, T_STR = 3, T_BIN = 4, T_ARR = 5;
const isBinary = (v) => v instanceof ArrayBuffer || ArrayBuffer.isView(v);
const keyType = (k) => typeof k === 'number' ? T_NUM : typeof k === 'string' ? T_STR : k instanceof Date ? T_DATE
    : Array.isArray(k) ? T_ARR : T_BIN;
const bytesOf = (k) => k instanceof ArrayBuffer ? new Uint8Array(k) : new Uint8Array(k.buffer, k.byteOffset, k.byteLength);

function cmp(a, b) {
    const ta = keyType(a), tb = keyType(b);
    if (ta !== tb) return ta < tb ? -1 : 1;
    switch (ta) {
    case T_NUM: return a < b ? -1 : a > b ? 1 : 0;
    case T_DATE: { const x = a.getTime(), y = b.getTime(); return x < y ? -1 : x > y ? 1 : 0; }
    case T_STR: return a < b ? -1 : a > b ? 1 : 0;
    case T_BIN: {
        const x = bytesOf(a), y = bytesOf(b), n = Math.min(x.length, y.length);
        for (let i = 0; i < n; i++) if (x[i] !== y[i]) return x[i] < y[i] ? -1 : 1;
        return x.length < y.length ? -1 : x.length > y.length ? 1 : 0;
    }
    default: {
        const n = Math.min(a.length, b.length);
        for (let i = 0; i < n; i++) { const c = cmp(a[i], b[i]); if (c) return c; }
        return a.length < b.length ? -1 : a.length > b.length ? 1 : 0;
    }
    }
}

/* "Convert a value to a key": a copy, or undefined when v is not a key. */
function toKey(v, seen) {
    if (typeof v === 'number') return Number.isNaN(v) ? undefined : v;
    if (typeof v === 'string') return v;
    if (v instanceof Date) { const t = v.getTime(); return Number.isNaN(t) ? undefined : new Date(t); }
    if (v instanceof ArrayBuffer) return v.slice(0);
    if (ArrayBuffer.isView(v)) return bytesOf(v).slice().buffer;
    if (Array.isArray(v)) {
        seen = seen || new Set();
        if (seen.has(v)) return undefined;
        seen.add(v);
        const out = [];
        for (let i = 0; i < v.length; i++) {
            if (!Object.prototype.hasOwnProperty.call(v, i)) return undefined;
            const k = toKey(v[i], seen);
            if (k === undefined) return undefined;
            out.push(k);
        }
        seen.delete(v);
        return out;
    }
    return undefined;
}
function needKey(v) {
    const k = toKey(v);
    if (k === undefined) throw domErr('The parameter is not a valid key', 'DataError');
    return k;
}
/* A key handed back to script: a fresh copy, so script cannot change the stored one. */
function outKey(k) {
    if (k instanceof Date) return new Date(k.getTime());
    if (k instanceof ArrayBuffer) return k.slice(0);
    if (Array.isArray(k)) return k.map(outKey);
    return k;
}

/* ---------------- Key paths ---------------- */

const identRe = /^[$A-Za-z_ª-￿][$0-9A-Za-z_ª-￿]*$/;
function validPath(p) {
    if (typeof p !== 'string') return false;
    return p === '' || p.split('.').every((s) => identRe.test(s));
}
function checkKeyPath(kp) {
    if (kp === null || kp === undefined) return null;
    if (Array.isArray(kp) || (typeof kp === 'object' && typeof kp[Symbol.iterator] === 'function')) {
        const list = Array.from(kp, String);
        if (!list.length || !list.every(validPath)) throw domErr('The keyPath is not valid', 'SyntaxError');
        return list;
    }
    const s = String(kp);
    if (!validPath(s)) throw domErr('The keyPath is not valid', 'SyntaxError');
    return s;
}
/* "Evaluate a key path on a value": the raw value at the path, or a
 * marker when a step is missing. */
const MISSING = {};
function walk(value, path) {
    if (path === '') return value;
    let v = value;
    for (const step of path.split('.')) {
        if (v === null || v === undefined) return MISSING;
        if (typeof v === 'string' && step === 'length') { v = v.length; continue; }
        if (Array.isArray(v) && step === 'length') { v = v.length; continue; }
        if (typeof v !== 'object') return MISSING;
        if (typeof G.Blob === 'function' && v instanceof G.Blob && (step === 'size' || step === 'type')) { v = v[step]; continue; }
        if (!Object.prototype.hasOwnProperty.call(v, step)) return MISSING;
        v = v[step];
    }
    return v;
}
/* The key at kp in value: a key, undefined (missing or not a key). */
function keyAt(value, kp, multi) {
    if (Array.isArray(kp)) {
        const out = [];
        for (const p of kp) {
            const v = walk(value, p);
            if (v === MISSING) return undefined;
            const k = toKey(v);
            if (k === undefined) return undefined;
            out.push(k);
        }
        return out;
    }
    const v = walk(value, kp);
    if (v === MISSING) return undefined;
    if (multi && Array.isArray(v)) {
        /* multiEntry: the valid, distinct keys in the array */
        const out = [];
        for (const e of v) {
            const k = toKey(e);
            if (k !== undefined && !out.some((o) => cmp(o, k) === 0)) out.push(k);
        }
        return out;
    }
    return toKey(v);
}
/* Puts a generated key into a value at a key path (autoIncrement). */
function injectKey(value, path, key) {
    const steps = path.split('.');
    let v = value;
    for (let i = 0; i < steps.length - 1; i++) {
        const s = steps[i];
        if (!Object.prototype.hasOwnProperty.call(v, s)) v[s] = {};
        v = v[s];
        if (v === null || typeof v !== 'object') return false;
    }
    v[steps[steps.length - 1]] = key;
    return true;
}
function canInject(value, path) {
    let v = value;
    const steps = path.split('.');
    for (let i = 0; i < steps.length - 1; i++) {
        if (v === null || typeof v !== 'object') return false;
        if (!Object.prototype.hasOwnProperty.call(v, steps[i])) return true;
        v = v[steps[i]];
    }
    return v !== null && typeof v === 'object';
}

/* ---------------- Key ranges ---------------- */

const RANGE = Symbol('range');
class IDBKeyRange {
    constructor() { throw new TypeError('Illegal constructor'); }
    get lower() { return outKey(this[RANGE].lower); }
    get upper() { return outKey(this[RANGE].upper); }
    get lowerOpen() { return this[RANGE].lowerOpen; }
    get upperOpen() { return this[RANGE].upperOpen; }
    includes(key) {
        if (!arguments.length) throw new TypeError('includes: 1 argument required');
        return inRange(this[RANGE], needKey(key));
    }
    static only(v) { const k = needKey(v); return makeRange(k, k, false, false); }
    static lowerBound(v, open = false) { return makeRange(needKey(v), undefined, !!open, true); }
    static upperBound(v, open = false) { return makeRange(undefined, needKey(v), true, !!open); }
    static bound(lo, hi, lo_open = false, hi_open = false) {
        const a = needKey(lo), b = needKey(hi), c = cmp(a, b);
        if (c > 0 || (c === 0 && (lo_open || hi_open))) throw domErr('The lower bound is above the upper bound', 'DataError');
        return makeRange(a, b, !!lo_open, !!hi_open);
    }
}
function makeRange(lower, upper, lowerOpen, upperOpen) {
    const r = Object.create(IDBKeyRange.prototype);
    Object.defineProperty(r, RANGE, { value: { lower, upper, lowerOpen, upperOpen } });
    return r;
}
function inRange(r, k) {
    if (r.lower !== undefined) { const c = cmp(r.lower, k); if (c > 0 || (c === 0 && r.lowerOpen)) return false; }
    if (r.upper !== undefined) { const c = cmp(k, r.upper); if (c > 0 || (c === 0 && r.upperOpen)) return false; }
    return true;
}
/* A query (a key or a range, or nothing) as a range; null for "all". */
function toRange(q, nullOk) {
    if (q instanceof IDBKeyRange) return q[RANGE];
    if (q === undefined || q === null) {
        if (nullOk) return null;
        throw domErr('No key or key range given', 'DataError');
    }
    const k = needKey(q);
    return { lower: k, upper: k, lowerOpen: false, upperOpen: false };
}

/* ---------------- Sorted arrays ---------------- */

/* The first index whose entry is >= key (by.0 then by.1 with pk). */
function lowerIndex(arr, key, pk) {
    let lo = 0, hi = arr.length;
    while (lo < hi) {
        const mid = (lo + hi) >> 1;
        let c = cmp(arr[mid][0], key);
        if (c === 0 && pk !== undefined) c = cmp(arr[mid][1], pk);
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}
function findRecord(store, key) {
    const i = lowerIndex(store.records, key);
    return i < store.records.length && cmp(store.records[i][0], key) === 0 ? i : -1;
}

/* ---------------- The databases ---------------- */

/* name -> { name, version, stores: Map(name -> store), conns: Set } */
const databases = new Map();
const newStore = (name, keyPath, autoIncrement) =>
    ({ name, keyPath, autoIncrement, current: 1, records: [], indexes: new Map(), deleted: false });

function indexEntries(ix, value, pk) {
    const k = keyAt(value, ix.keyPath, ix.multiEntry);
    if (k === undefined) return [];
    if (ix.multiEntry && !Array.isArray(ix.keyPath) && Array.isArray(k)) return k.map((x) => [x, pk]);
    return [[k, pk]];
}
function indexRemove(ix, pk) {
    ix.entries = ix.entries.filter((e) => cmp(e[1], pk) !== 0);
}
function indexAdd(ix, list) {
    for (const e of list) ix.entries.splice(lowerIndex(ix.entries, e[0], e[1]), 0, e);
}
/* Whether putting value under pk breaks a unique index. */
function uniqueClash(store, value, pk) {
    for (const ix of store.indexes.values()) {
        if (!ix.unique) continue;
        for (const [k] of indexEntries(ix, value, pk)) {
            const i = lowerIndex(ix.entries, k);
            for (let j = i; j < ix.entries.length && cmp(ix.entries[j][0], k) === 0; j++)
                if (cmp(ix.entries[j][1], pk) !== 0) return true;
        }
    }
    return false;
}
function storeRecord(store, key, value) {
    const i = lowerIndex(store.records, key);
    const exists = i < store.records.length && cmp(store.records[i][0], key) === 0;
    if (exists) {
        store.records[i] = [key, value];
        for (const ix of store.indexes.values()) indexRemove(ix, key);
    } else {
        store.records.splice(i, 0, [key, value]);
    }
    for (const ix of store.indexes.values()) indexAdd(ix, indexEntries(ix, value, key));
}
function deleteRange(store, r) {
    const keep = [], gone = [];
    for (const rec of store.records) (r === null || inRange(r, rec[0]) ? gone : keep).push(rec);
    store.records = keep;
    if (gone.length)
        for (const ix of store.indexes.values())
            ix.entries = ix.entries.filter((e) => !gone.some((g) => cmp(g[0], e[1]) === 0));
}

/* ---------------- DOMStringList ---------------- */

/* The document's DOMStringList (prelude_window.js); a worker has none,
 * so one is made here. */
const DOMStringList = typeof G.DOMStringList === 'function' ? G.DOMStringList : class DOMStringList {
    constructor() { throw new TypeError('Illegal constructor'); }
    item(i) { i >>>= 0; return i < this.length ? this[i] : null; }
    contains(s) { return Array.prototype.includes.call(this, String(s)); }
    [Symbol.iterator]() { return Array.prototype[Symbol.iterator].call(this); }
};
function stringList(names) {
    const l = [...names].sort();
    const o = Object.create(DOMStringList.prototype);
    l.forEach((v, i) => Object.defineProperty(o, i, { value: v, enumerable: true }));
    Object.defineProperty(o, 'length', { value: l.length });
    return Object.freeze(o);
}

/* ---------------- Events ---------------- */

class IDBVersionChangeEvent extends H.Event {
    constructor(type, init) {
        super(type, init);
        this._old = init && init.oldVersion !== undefined ? Number(init.oldVersion) : 0;
        this._new = init && init.newVersion !== undefined && init.newVersion !== null ? Number(init.newVersion) : null;
    }
    get oldVersion() { return this._old; }
    get newVersion() { return this._new; }
}

/* ---------------- Requests ---------------- */

const R = Symbol('idb');
class IDBRequest extends H.EventTarget {
    constructor() { throw new TypeError('Illegal constructor'); }
    get result() {
        const s = this[R];
        if (s.readyState !== 'done') throw domErr('The request has not finished', 'InvalidStateError');
        return s.result;
    }
    get error() {
        const s = this[R];
        if (s.readyState !== 'done') throw domErr('The request has not finished', 'InvalidStateError');
        return s.error;
    }
    get source() { return this[R].source; }
    get transaction() { return this[R].tx ? this[R].tx.obj : null; }
    get readyState() { return this[R].readyState; }
}
H.defineHandlers(IDBRequest.prototype, ['success', 'error']);
class IDBOpenDBRequest extends IDBRequest {}
H.defineHandlers(IDBOpenDBRequest.prototype, ['blocked', 'upgradeneeded']);

function newRequest(C, source, tx) {
    const r = Object.create(C.prototype);
    Object.defineProperty(r, R, { value: { readyState: 'pending', result: undefined, error: null, source, tx } });
    if (tx) H.eventParents.set(r, tx.obj);
    return r;
}

/* ---------------- Transactions ---------------- */

class IDBTransaction extends H.EventTarget {
    constructor() { throw new TypeError('Illegal constructor'); }
    get objectStoreNames() {
        const t = this[R];
        return stringList(t.mode === 'versionchange' ? t.db.data.stores.keys() : t.scope);
    }
    get mode() { return this[R].mode; }
    get durability() { return this[R].durability; }
    get db() { return this[R].db.obj; }
    get error() { return this[R].error; }
    objectStore(name) {
        const t = this[R];
        if (t.state === 'finished') throw domErr('The transaction has finished', 'InvalidStateError');
        name = String(name);
        if (!(t.mode === 'versionchange' ? t.db.data.stores.has(name) : t.scope.includes(name)))
            throw domErr("No object store named '" + name + "' in this transaction", 'NotFoundError');
        const store = t.db.data.stores.get(name);
        if (!store) throw domErr("No object store named '" + name + "'", 'NotFoundError');
        let o = t.storeObjs.get(store);
        if (!o) { o = makeStore(store, t); t.storeObjs.set(store, o); }
        return o;
    }
    abort() {
        const t = this[R];
        if (t.state === 'committing' || t.state === 'finished') throw domErr('The transaction has finished', 'InvalidStateError');
        abortTx(t, null);
    }
    commit() {
        const t = this[R];
        if (t.state !== 'active') throw domErr('The transaction is not active', 'InvalidStateError');
        t.commitAsked = true;
        t.state = 'inactive';
        schedule(t);
    }
}
H.defineHandlers(IDBTransaction.prototype, ['abort', 'complete', 'error']);

let txQueue = []; /* transactions waiting to run, in the order they were made */
function newTx(db, scope, mode, durability) {
    const obj = Object.create(IDBTransaction.prototype);
    const t = { obj, db, scope, mode, durability, state: 'active', requests: [], running: false, error: null,
        saved: new Map(), savedShape: null, storeObjs: new Map(), started: false, commitAsked: false, onDone: [] };
    Object.defineProperty(obj, R, { value: t });
    H.eventParents.set(obj, db.obj);
    db.txs.add(t);
    txQueue.push(t);
    /* inactive when the task that made it ends */
    N.queueTask(() => { if (t.state === 'active') { t.state = 'inactive'; schedule(t); } });
    schedule(t);
    return t;
}
/* Readwrite transactions with overlapping scopes run one at a time. */
function canStart(t) {
    for (const o of txQueue) {
        if (o === t) return true;
        if (!o.started && o.mode === 'readonly' && t.mode === 'readonly') continue;
        if (o.scope.some((s) => t.scope.includes(s)) || o.mode === 'versionchange' || t.mode === 'versionchange')
            return false;
    }
    return true;
}
function schedule(t) {
    if (t.running || t.state === 'finished') return;
    t.running = true;
    N.queueTask(() => { t.running = false; step(t); });
}
function step(t) {
    if (t.state === 'finished') return;
    if (!t.started) {
        if (!canStart(t)) return; /* scheduled again when an earlier one ends */
        t.started = true;
    }
    if (t.requests.length) {
        const job = t.requests.shift();
        runRequest(t, job);
        if (t.state !== 'finished') schedule(t);
        return;
    }
    if (t.state === 'inactive') commitTx(t);
}
/* Saves a store's arrays before a transaction first changes them. */
function save(t, store) {
    if (t.mode === 'readonly' || t.saved.has(store)) return;
    const ix = new Map();
    for (const [n, i] of store.indexes) ix.set(n, i.entries.slice());
    t.saved.set(store, { records: store.records.slice(), current: store.current, ix });
}
function finishTx(t) {
    t.state = 'finished';
    t.db.txs.delete(t);
    txQueue = txQueue.filter((o) => o !== t);
    for (const o of txQueue) schedule(o);
    for (const f of t.onDone.splice(0)) f();
}
function commitTx(t) {
    t.state = 'committing';
    finishTx(t);
    H.fireSimple(t.obj, 'complete');
    if (t.mode === 'versionchange') t.db.upgrading = null;
    t.afterComplete && t.afterComplete();
}
function abortTx(t, error) {
    if (t.state === 'finished') return;
    /* put back what it changed */
    for (const [store, s] of t.saved) {
        store.records = s.records;
        store.current = s.current;
        for (const [n, e] of s.ix) { const i = store.indexes.get(n); if (i) i.entries = e; }
    }
    if (t.savedShape) {
        const d = t.db.data;
        d.stores = t.savedShape.stores;
        d.version = t.savedShape.version;
        for (const [store, meta] of t.savedShape.meta) { store.indexes = meta.indexes; store.name = meta.name; store.deleted = false; }
        t.db.version = d.version;
    }
    t.error = error;
    const pending = t.requests.splice(0);
    finishTx(t);
    t.state = 'finished';
    N.queueTask(() => {
        for (const job of pending) {
            const s = job.req[R];
            s.readyState = 'done';
            s.result = undefined;
            s.error = domErr('The transaction was aborted', 'AbortError');
            H.fire(job.req, new H.Event('error', { bubbles: true, cancelable: true }));
        }
        H.fire(t.obj, new H.Event('abort', { bubbles: true }));
        if (t.mode === 'versionchange') t.db.upgrading = null;
        t.afterAbort && t.afterAbort();
    });
}
function checkActive(t) {
    if (t.state !== 'active') {
        throw domErr(t.state === 'finished' ? 'The transaction has finished' : 'The transaction is not active',
            'TransactionInactiveError');
    }
}
function checkWritable(t) {
    if (t.mode === 'readonly') throw domErr('The transaction is read-only', 'ReadOnlyError');
}
/* Queues an operation; op() returns the result or throws a DOMException. */
function request(t, source, op, req) {
    req = req || newRequest(IDBRequest, source, t);
    t.requests.push({ req, op });
    schedule(t);
    return req;
}
function runRequest(t, job) {
    const s = job.req[R];
    let result, error = null;
    try { result = job.op(); } catch (e) { error = e instanceof DOMException ? e : domErr(String(e && e.message || e), 'UnknownError'); }
    s.readyState = 'done';
    /* active while the event is dispatched and until the task ends, so
     * script's promise callbacks may place the next request */
    t.state = 'active';
    if (error) {
        s.result = undefined;
        s.error = error;
        const ev = new H.Event('error', { bubbles: true, cancelable: true });
        let threw = false;
        try { H.fire(job.req, ev); } catch (e) { threw = true; }
        if (t.state !== 'finished' && (!ev.defaultPrevented || threw)) {
            abortTx(t, error);
            return;
        }
    } else {
        s.result = result;
        s.error = null;
        H.fire(job.req, new H.Event('success'));
    }
    if (t.state === 'active') N.queueTask(() => { if (t.state === 'active') { t.state = 'inactive'; schedule(t); } });
}

/* ---------------- Object stores ---------------- */

class IDBObjectStore {
    constructor() { throw new TypeError('Illegal constructor'); }
    get name() { return this[R].store.name; }
    set name(v) {
        const { store, tx } = this[R];
        v = String(v);
        if (tx.mode !== 'versionchange') throw domErr('Not in an upgrade', 'InvalidStateError');
        checkActive(tx);
        if (v === store.name) return;
        const stores = tx.db.data.stores;
        if (stores.has(v)) throw domErr("An object store named '" + v + "' exists", 'ConstraintError');
        stores.delete(store.name);
        store.name = v;
        stores.set(v, store);
    }
    get keyPath() { const k = this[R].store.keyPath; return Array.isArray(k) ? k.slice() : k; }
    get indexNames() { return stringList(this[R].store.indexes.keys()); }
    get transaction() { return this[R].tx.obj; }
    get autoIncrement() { return this[R].store.autoIncrement; }
    put(value, key) { return addOrPut(this, value, key, arguments.length > 1, false); }
    add(value, key) { return addOrPut(this, value, key, arguments.length > 1, true); }
    delete(query) {
        const { store, tx } = live(this);
        checkWritable(tx);
        const r = toRange(query, false);
        return request(tx, this, () => { save(tx, store); deleteRange(store, r); return undefined; });
    }
    clear() {
        const { store, tx } = live(this);
        checkWritable(tx);
        return request(tx, this, () => {
            save(tx, store);
            store.records = [];
            for (const ix of store.indexes.values()) ix.entries = [];
            return undefined;
        });
    }
    get(query) {
        const { store, tx } = live(this);
        const r = toRange(query, false);
        return request(tx, this, () => {
            const rec = store.records.find((x) => inRange(r, x[0]));
            return rec ? H.structuredClone(rec[1]) : undefined;
        });
    }
    getKey(query) {
        const { store, tx } = live(this);
        const r = toRange(query, false);
        return request(tx, this, () => { const rec = store.records.find((x) => inRange(r, x[0])); return rec ? outKey(rec[0]) : undefined; });
    }
    getAll(query, count) { return getAllOf(this, query, count, false); }
    getAllKeys(query, count) { return getAllOf(this, query, count, true); }
    count(query) {
        const { store, tx } = live(this);
        const r = toRange(query, true);
        return request(tx, this, () => store.records.reduce((n, x) => n + (r === null || inRange(r, x[0]) ? 1 : 0), 0));
    }
    openCursor(query, dir) { return openCursor(this, null, query, dir, false); }
    openKeyCursor(query, dir) { return openCursor(this, null, query, dir, true); }
    index(name) {
        const { store, tx } = this[R];
        if (store.deleted) throw domErr('The object store was deleted', 'InvalidStateError');
        if (tx.state === 'finished') throw domErr('The transaction has finished', 'InvalidStateError');
        const ix = store.indexes.get(String(name));
        if (!ix) throw domErr("No index named '" + name + "'", 'NotFoundError');
        let o = this[R].ixObjs.get(ix);
        if (!o) { o = makeIndex(ix, this); this[R].ixObjs.set(ix, o); }
        return o;
    }
    createIndex(name, keyPath, opts) {
        const { store, tx } = this[R];
        if (tx.mode !== 'versionchange') throw domErr('Not in an upgrade', 'InvalidStateError');
        if (store.deleted) throw domErr('The object store was deleted', 'InvalidStateError');
        checkActive(tx);
        name = String(name);
        if (store.indexes.has(name)) throw domErr("An index named '" + name + "' exists", 'ConstraintError');
        const kp = checkKeyPath(keyPath);
        if (kp === null) throw domErr('The keyPath is not valid', 'SyntaxError');
        const unique = !!(opts && opts.unique), multiEntry = !!(opts && opts.multiEntry);
        if (multiEntry && Array.isArray(kp)) throw domErr('multiEntry with an array keyPath', 'InvalidAccessError');
        const ix = { name, keyPath: kp, unique, multiEntry, entries: [], deleted: false };
        save(tx, store);
        store.indexes.set(name, ix);
        /* filled from the records now; a unique clash aborts the upgrade */
        for (const [pk, v] of store.records) indexAdd(ix, indexEntries(ix, v, pk));
        if (unique)
            for (let i = 1; i < ix.entries.length; i++)
                if (cmp(ix.entries[i - 1][0], ix.entries[i][0]) === 0) {
                    N.queueTask(() => abortTx(tx, domErr('The index is unique and the store has duplicates', 'ConstraintError')));
                    break;
                }
        return this.index(name);
    }
    deleteIndex(name) {
        const { store, tx } = this[R];
        if (tx.mode !== 'versionchange') throw domErr('Not in an upgrade', 'InvalidStateError');
        checkActive(tx);
        const ix = store.indexes.get(String(name));
        if (!ix) throw domErr("No index named '" + name + "'", 'NotFoundError');
        ix.deleted = true;
        store.indexes.delete(String(name));
    }
}
function makeStore(store, tx) {
    const o = Object.create(IDBObjectStore.prototype);
    Object.defineProperty(o, R, { value: { store, tx, ixObjs: new Map() } });
    return o;
}
/* The store and transaction, checked usable for a request. */
function live(o) {
    const s = o[R];
    if (!s) throw new TypeError('Illegal invocation');
    const store = s.ix ? s.ix.store[R].store : s.store;
    const tx = s.ix ? s.ix.store[R].tx : s.tx;
    if (store.deleted || (s.ix && s.ix.ix.deleted)) throw domErr('The object store or index was deleted', 'InvalidStateError');
    checkActive(tx);
    return { store, tx, ix: s.ix ? s.ix.ix : null };
}
function addOrPut(o, value, key, haveKey, noOverwrite) {
    const { store, tx } = live(o);
    checkWritable(tx);
    if (haveKey && store.keyPath !== null)
        throw domErr('The object store uses in-line keys and the key parameter was provided', 'DataError');
    if (!haveKey && store.keyPath === null && !store.autoIncrement)
        throw domErr('The object store uses out-of-line keys and has no key generator', 'DataError');
    const clone = H.structuredClone(value);
    let k;
    if (haveKey) k = needKey(key);
    else if (store.keyPath !== null) {
        k = keyAt(clone, store.keyPath, false);
        const missing = walk(clone, Array.isArray(store.keyPath) ? '' : store.keyPath) === MISSING;
        if (k === undefined && !(store.autoIncrement && missing && !Array.isArray(store.keyPath)))
            throw domErr('The key at the keyPath is not valid', 'DataError');
        if (k === undefined && !canInject(clone, store.keyPath))
            throw domErr('The key cannot be set on the value', 'DataError');
    }
    return request(tx, o, () => {
        let key2 = k;
        if (key2 === undefined) {
            if (store.current > 9007199254740992) throw domErr('The key generator has run out', 'ConstraintError');
            save(tx, store);
            key2 = store.current++;
            if (store.keyPath !== null) injectKey(clone, store.keyPath, key2);
        } else if (store.autoIncrement && typeof key2 === 'number' && key2 >= store.current) {
            save(tx, store);
            store.current = Math.floor(key2) + 1;
        }
        if (noOverwrite && findRecord(store, key2) >= 0) throw domErr('A record with the key exists', 'ConstraintError');
        if (uniqueClash(store, clone, key2)) throw domErr('A unique index already has the key', 'ConstraintError');
        save(tx, store);
        storeRecord(store, key2, clone);
        return outKey(key2);
    });
}
function getAllOf(o, query, count, keys) {
    const { store, tx, ix } = live(o);
    let r, n;
    if (query && typeof query === 'object' && !(query instanceof IDBKeyRange) && !Array.isArray(query) &&
        !isBinary(query) && !(query instanceof Date) && ('query' in query || 'count' in query || 'direction' in query)) {
        r = toRange(query.query, true);
        n = query.count;
    } else {
        r = toRange(query, true);
        n = count;
    }
    n = n === undefined || n === null ? 0 : Number(n) >>> 0;
    return request(tx, o, () => {
        const out = [];
        if (ix) {
            for (const [k, pk] of ix.entries) {
                if (r !== null && !inRange(r, k)) continue;
                if (keys) out.push(outKey(pk));
                else { const i = findRecord(store, pk); if (i >= 0) out.push(H.structuredClone(store.records[i][1])); }
                if (n && out.length >= n) break;
            }
        } else {
            for (const [k, v] of store.records) {
                if (r !== null && !inRange(r, k)) continue;
                out.push(keys ? outKey(k) : H.structuredClone(v));
                if (n && out.length >= n) break;
            }
        }
        return out;
    });
}

/* ---------------- Indexes ---------------- */

class IDBIndex {
    constructor() { throw new TypeError('Illegal constructor'); }
    get name() { return this[R].ix.ix.name; }
    set name(v) {
        const s = this[R], ix = s.ix.ix, store = s.store[R].store, tx = s.store[R].tx;
        if (tx.mode !== 'versionchange') throw domErr('Not in an upgrade', 'InvalidStateError');
        checkActive(tx);
        v = String(v);
        if (v === ix.name) return;
        if (store.indexes.has(v)) throw domErr("An index named '" + v + "' exists", 'ConstraintError');
        store.indexes.delete(ix.name);
        ix.name = v;
        store.indexes.set(v, ix);
    }
    get objectStore() { return this[R].store; }
    get keyPath() { const k = this[R].ix.ix.keyPath; return Array.isArray(k) ? k.slice() : k; }
    get multiEntry() { return this[R].ix.ix.multiEntry; }
    get unique() { return this[R].ix.ix.unique; }
    get(query) {
        const { store, tx, ix } = live(this);
        const r = toRange(query, false);
        return request(tx, this, () => {
            const e = ix.entries.find((x) => inRange(r, x[0]));
            if (!e) return undefined;
            const i = findRecord(store, e[1]);
            return i >= 0 ? H.structuredClone(store.records[i][1]) : undefined;
        });
    }
    getKey(query) {
        const { tx, ix } = live(this);
        const r = toRange(query, false);
        return request(tx, this, () => { const e = ix.entries.find((x) => inRange(r, x[0])); return e ? outKey(e[1]) : undefined; });
    }
    getAll(query, count) { return getAllOf(this, query, count, false); }
    getAllKeys(query, count) { return getAllOf(this, query, count, true); }
    count(query) {
        const { tx, ix } = live(this);
        const r = toRange(query, true);
        return request(tx, this, () => ix.entries.reduce((n, x) => n + (r === null || inRange(r, x[0]) ? 1 : 0), 0));
    }
    openCursor(query, dir) { return openCursor(this[R].store, this, query, dir, false); }
    openKeyCursor(query, dir) { return openCursor(this[R].store, this, query, dir, true); }
}
/* An index object's state: ix.ix the index, ix.store and store its
 * store's object (live() finds the store and transaction through it). */
function makeIndex(ix, storeObj) {
    const o = Object.create(IDBIndex.prototype);
    Object.defineProperty(o, R, { value: { ix: { ix, store: storeObj }, store: storeObj } });
    return o;
}

/* ---------------- Cursors ---------------- */

class IDBCursor {
    constructor() { throw new TypeError('Illegal constructor'); }
    get source() { return this[R].source; }
    get direction() { return this[R].dir; }
    get key() { return outKey(this[R].key); }
    get primaryKey() { return outKey(this[R].pk); }
    get request() { return this[R].req; }
    advance(count) {
        count = Number(count);
        if (!(count >= 1) || count > 4294967295) throw new TypeError('advance: the count must be at least 1');
        moveOn(this, (c) => { c.skip = Math.floor(count); });
    }
    continue(key) {
        const c = this[R];
        let k;
        if (key !== undefined) {
            k = needKey(key);
            const d = cmp(k, c.key);
            if ((c.dir.startsWith('next') && d <= 0) || (c.dir.startsWith('prev') && d >= 0))
                throw domErr('The key is not beyond the cursor', 'DataError');
        }
        moveOn(this, (s) => { s.target = k; });
    }
    continuePrimaryKey(key, pk) {
        const c = this[R];
        if (!c.ixObj) throw domErr('Not an index cursor', 'InvalidAccessError');
        if (c.dir.endsWith('unique')) throw domErr('A unique cursor', 'InvalidAccessError');
        const k = needKey(key), p = needKey(pk);
        const d = cmp(k, c.key);
        if ((c.dir === 'next' && (d < 0 || (d === 0 && cmp(p, c.pk) <= 0))) ||
            (c.dir === 'prev' && (d > 0 || (d === 0 && cmp(p, c.pk) >= 0))))
            throw domErr('The key is not beyond the cursor', 'DataError');
        moveOn(this, (s) => { s.target = k; s.targetPk = p; });
    }
    update(value) {
        const c = this[R];
        const { store, tx } = live(c.storeObj);
        checkWritable(tx);
        if (!c.gotValue || c.keyOnly) throw domErr('The cursor has no value', 'InvalidStateError');
        const clone = H.structuredClone(value);
        if (store.keyPath !== null) {
            const k = keyAt(clone, store.keyPath, false);
            if (k === undefined || cmp(k, c.pk) !== 0) throw domErr('The key at the keyPath changed', 'DataError');
        }
        const pk = c.pk;
        return request(tx, this, () => {
            if (uniqueClash(store, clone, pk)) throw domErr('A unique index already has the key', 'ConstraintError');
            save(tx, store);
            storeRecord(store, pk, clone);
            return outKey(pk);
        });
    }
    delete() {
        const c = this[R];
        const { store, tx } = live(c.storeObj);
        checkWritable(tx);
        if (!c.gotValue || c.keyOnly) throw domErr('The cursor has no value', 'InvalidStateError');
        const pk = c.pk;
        return request(tx, this, () => {
            save(tx, store);
            deleteRange(store, { lower: pk, upper: pk, lowerOpen: false, upperOpen: false });
            return undefined;
        });
    }
}
class IDBCursorWithValue extends IDBCursor {
    get value() { return this[R].value; }
}
function moveOn(cursor, set) {
    const c = cursor[R];
    live(c.storeObj);
    if (!c.gotValue) throw domErr('The cursor is moving or finished', 'InvalidStateError');
    c.gotValue = false;
    c.skip = 1;
    c.target = undefined;
    c.targetPk = undefined;
    set(c);
    const s = c.req[R];
    s.readyState = 'pending';
    request(c.tx, c.source, () => iterate(c), c.req);
}
function openCursor(storeObj, ixObj, query, dir, keyOnly) {
    const src = ixObj || storeObj;
    const { tx } = live(src);
    dir = dir === undefined ? 'next' : String(dir);
    if (!['next', 'nextunique', 'prev', 'prevunique'].includes(dir))
        throw new TypeError("The direction '" + dir + "' is not valid");
    const range = toRange(query, true);
    const obj = Object.create((keyOnly ? IDBCursor : IDBCursorWithValue).prototype);
    const c = { storeObj, ixObj, source: src, dir, range, keyOnly, tx, key: undefined, pk: undefined, value: undefined,
        gotValue: false, skip: 1, target: undefined, targetPk: undefined, started: false, req: null, obj };
    Object.defineProperty(obj, R, { value: c });
    c.req = request(tx, src, () => iterate(c));
    return c.req;
}
/* Moves the cursor to the next record it may show; the cursor, or null at the end. */
function iterate(c) {
    const store = c.storeObj[R].store;
    const ix = c.ixObj ? c.ixObj[R].ix.ix : null;
    const fwd = c.dir.startsWith('next'), unique = c.dir.endsWith('unique');
    /* the list the cursor walks: [key, primaryKey] */
    const list = ix ? ix.entries : store.records;
    const pkOf = (e) => (ix ? e[1] : e[0]);
    let i;
    if (!c.started) i = fwd ? 0 : list.length - 1;
    else if (fwd) {
        i = ix ? lowerIndex(list, c.key, c.pk) : lowerIndex(list, c.key);
        while (i < list.length && cmp(list[i][0], c.key) === 0 && (!ix || cmp(pkOf(list[i]), c.pk) <= 0)) i++;
    } else {
        i = (ix ? lowerIndex(list, c.key, c.pk) : lowerIndex(list, c.key)) - 1;
        while (i >= 0 && cmp(list[i][0], c.key) === 0 && ix && cmp(pkOf(list[i]), c.pk) >= 0) i--;
    }
    let skip = c.started ? c.skip : 1;
    let last = c.started ? c.key : undefined;
    for (; fwd ? i < list.length : i >= 0; i += fwd ? 1 : -1) {
        const e = list[i], k = e[0];
        if (c.range && !inRange(c.range, k)) {
            /* past the far end of the range stops the walk */
            if (fwd ? c.range.upper !== undefined && cmp(k, c.range.upper) > 0 : c.range.lower !== undefined && cmp(k, c.range.lower) < 0) break;
            continue;
        }
        if (c.target !== undefined) {
            const d = cmp(k, c.target);
            if (fwd ? d < 0 : d > 0) continue;
            if (d === 0 && c.targetPk !== undefined && (fwd ? cmp(pkOf(e), c.targetPk) < 0 : cmp(pkOf(e), c.targetPk) > 0)) continue;
        }
        if (unique && last !== undefined && cmp(k, last) === 0) continue;
        if (unique && !fwd && i > 0 && cmp(list[i - 1][0], k) === 0) {
            /* prevunique shows the first record of each key */
            continue;
        }
        if (--skip > 0) { last = k; continue; } /* advance(n): pass this one */
        c.started = true;
        c.key = k;
        c.pk = pkOf(e);
        if (!c.keyOnly) {
            const r = ix ? store.records[findRecord(store, c.pk)] : e;
            c.value = r ? H.structuredClone(r[1]) : undefined;
        }
        c.gotValue = true;
        return c.obj;
    }
    c.started = true;
    c.key = c.pk = c.value = undefined;
    return null;
}

/* ---------------- Connections ---------------- */

class IDBDatabase extends H.EventTarget {
    constructor() { throw new TypeError('Illegal constructor'); }
    get name() { return this[R].data.name; }
    get version() { return this[R].version; }
    get objectStoreNames() { return stringList(this[R].data.stores.keys()); }
    createObjectStore(name, opts) {
        const db = this[R], t = db.upgrading;
        if (!t) throw domErr('Not in an upgrade', 'InvalidStateError');
        checkActive(t);
        name = String(name);
        const kp = checkKeyPath(opts && opts.keyPath !== undefined ? opts.keyPath : null);
        const ai = !!(opts && opts.autoIncrement);
        if (db.data.stores.has(name)) throw domErr("An object store named '" + name + "' exists", 'ConstraintError');
        if (ai && (kp === '' || Array.isArray(kp))) throw domErr('autoIncrement with an empty or array keyPath', 'InvalidAccessError');
        const store = newStore(name, kp, ai);
        db.data.stores.set(name, store);
        return t.obj.objectStore(name);
    }
    deleteObjectStore(name) {
        const db = this[R], t = db.upgrading;
        if (!t) throw domErr('Not in an upgrade', 'InvalidStateError');
        checkActive(t);
        const store = db.data.stores.get(String(name));
        if (!store) throw domErr("No object store named '" + name + "'", 'NotFoundError');
        store.deleted = true;
        db.data.stores.delete(String(name));
    }
    transaction(names, mode, opts) {
        const db = this[R];
        if (db.upgrading) throw domErr('An upgrade is running', 'InvalidStateError');
        if (db.closePending) throw domErr('The connection is closing', 'InvalidStateError');
        const list = typeof names === 'string' ? [names] : Array.from(names, String);
        const scope = [...new Set(list)].sort();
        for (const n of scope)
            if (!db.data.stores.has(n)) throw domErr("No object store named '" + n + "'", 'NotFoundError');
        mode = mode === undefined ? 'readonly' : String(mode);
        if (mode !== 'readonly' && mode !== 'readwrite') throw new TypeError("The mode '" + mode + "' is not valid");
        if (!scope.length) throw domErr('No object store named', 'InvalidAccessError');
        const durability = opts && ['strict', 'relaxed', 'default'].includes(opts.durability) ? opts.durability : 'default';
        return newTx(db, scope, mode, durability).obj;
    }
    close() {
        const db = this[R];
        db.closePending = true;
        db.data.conns.delete(db);
        retryOpens();
    }
}
H.defineHandlers(IDBDatabase.prototype, ['abort', 'close', 'error', 'versionchange']);

/* ---------------- The factory ---------------- */

const opens = []; /* open and delete requests waiting for connections to close */

/* Every document and worker may open databases, opaque origins too:
 * the databases are its own, in memory, and no other document sees
 * them, so there is nothing to keep apart. */

class IDBFactory {
    constructor() { throw new TypeError('Illegal constructor'); }
    open(name, version) {
        if (!arguments.length) throw new TypeError('open: 1 argument required');
        name = String(name);
        if (version !== undefined) {
            version = Number(version);
            if (!Number.isFinite(version) || version < 1 || version > 9007199254740991)
                throw new TypeError('The version must be a positive integer');
            version = Math.floor(version);
        }
        const req = newRequest(IDBOpenDBRequest, null, null);
        N.queueTask(() => openStep({ kind: 'open', name, version, req, warned: false }));
        return req;
    }
    deleteDatabase(name) {
        if (!arguments.length) throw new TypeError('deleteDatabase: 1 argument required');
        const req = newRequest(IDBOpenDBRequest, null, null);
        N.queueTask(() => openStep({ kind: 'delete', name: String(name), req, warned: false }));
        return req;
    }
    databases() {
        return Promise.resolve([...databases.values()].map((d) => ({ name: d.name, version: d.version })));
    }
    cmp(a, b) {
        if (arguments.length < 2) throw new TypeError('cmp: 2 arguments required');
        return cmp(needKey(a), needKey(b));
    }
}

function done(req, result, error) {
    const s = req[R];
    s.readyState = 'done';
    s.result = result;
    s.error = error || null;
    if (error) H.fire(req, new H.Event('error', { bubbles: true, cancelable: true }));
    else H.fire(req, new H.Event('success'));
}
/* Other connections are asked to close (versionchange); while any stays
 * open the request waits, after a blocked event. */
function waitForOthers(job, data, newVersion, except) {
    const others = [...data.conns].filter((c) => c !== except && !c.closePending);
    if (!job.warned) {
        for (const c of others)
            H.fire(c.obj, new IDBVersionChangeEvent('versionchange', { oldVersion: data.version, newVersion }));
    }
    const still = [...data.conns].filter((c) => c !== except && !c.closePending);
    if (still.length) {
        if (!job.warned) H.fire(job.req, new IDBVersionChangeEvent('blocked', { oldVersion: data.version, newVersion }));
        job.warned = true;
        if (!opens.includes(job)) opens.push(job);
        return true;
    }
    return false;
}
function retryOpens() {
    const list = opens.splice(0);
    for (const job of list) N.queueTask(() => openStep(job));
}
function openStep(job) {
    let data = databases.get(job.name);
    if (job.kind === 'delete') {
        if (!data) { done(job.req, undefined); return; }
        if (waitForOthers(job, data, null, null)) return;
        const old = data.version;
        databases.delete(job.name);
        const s = job.req[R];
        s.readyState = 'done';
        s.result = undefined;
        H.fire(job.req, new IDBVersionChangeEvent('success', { oldVersion: old, newVersion: null }));
        return;
    }
    if (!data) data = { name: job.name, version: 0, stores: new Map(), conns: new Set() };
    const version = job.version === undefined ? Math.max(1, data.version) : job.version;
    if (version < data.version) { done(job.req, undefined, domErr('The requested version is less than the existing version', 'VersionError')); return; }
    const conn = { data, version: data.version, txs: new Set(), upgrading: null, closePending: false, obj: null };
    conn.obj = Object.create(IDBDatabase.prototype);
    Object.defineProperty(conn.obj, R, { value: conn });
    if (version > data.version) {
        if (data.conns.size && waitForOthers(job, data, version, null)) return;
        databases.set(job.name, data);
        data.conns.add(conn);
        const old = data.version;
        /* the upgrade: its own transaction over every store */
        const meta = new Map();
        for (const st of data.stores.values()) meta.set(st, { indexes: new Map(st.indexes), name: st.name });
        data.version = version;
        conn.version = version;
        const t = newTx(conn, [...data.stores.keys()], 'versionchange', 'default');
        t.savedShape = { stores: new Map(data.stores), version: old, meta };
        for (const st of data.stores.values()) save(t, st);
        conn.upgrading = t;
        const s = job.req[R];
        s.tx = t;
        s.result = conn.obj;
        s.readyState = 'done';
        t.state = 'active';
        H.fire(job.req, new IDBVersionChangeEvent('upgradeneeded', { oldVersion: old, newVersion: version }));
        t.afterComplete = () => { s.tx = null; done(job.req, conn.obj); };
        t.afterAbort = () => {
            s.tx = null;
            data.conns.delete(conn);
            conn.closePending = true;
            if (old === 0) databases.delete(job.name);
            done(job.req, undefined, domErr('The upgrade was aborted', 'AbortError'));
        };
        return;
    }
    databases.set(job.name, data);
    data.conns.add(conn);
    done(job.req, conn.obj);
}

const indexedDB = Object.create(IDBFactory.prototype);

for (const C of [IDBFactory, IDBRequest, IDBOpenDBRequest, IDBDatabase, IDBTransaction, IDBObjectStore, IDBIndex,
    IDBCursor, IDBCursorWithValue, IDBKeyRange, IDBVersionChangeEvent])
    Object.defineProperty(C.prototype, Symbol.toStringTag, { value: C.name, configurable: true });

return { indexedDB, IDBFactory, IDBRequest, IDBOpenDBRequest, IDBDatabase, IDBTransaction, IDBObjectStore, IDBIndex,
    IDBCursor, IDBCursorWithValue, IDBKeyRange, IDBVersionChangeEvent, DOMStringList };
})
