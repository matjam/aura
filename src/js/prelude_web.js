
/* Web APIs that need no document, shared by the page prelude and the
 * worker prelude (prelude.js, this file, prelude_net.js, then
 * prelude_worker.js for a worker): timers, structuredClone and the
 * message encoding between runtimes, MessageChannel, crypto, URL,
 * URLSearchParams, TextEncoder and TextDecoder, the module loader, form
 * encoding. */

/* timers */
const timerFn = (fn) => (typeof fn === 'function' ? fn : str(fn));
defGlobal('setTimeout', function setTimeout(fn, ms, ...args) { return N.setTimeout(timerFn(fn), ms, ...args); });
defGlobal('setInterval', function setInterval(fn, ms, ...args) { return N.setInterval(timerFn(fn), ms, ...args); });
defGlobal('clearTimeout', function clearTimeout(id) { if (id !== undefined) N.clearTimer(Number(id) >>> 0); });
defGlobal('clearInterval', function clearInterval(id) { if (id !== undefined) N.clearTimer(Number(id) >>> 0); });
defGlobal('queueMicrotask', function queueMicrotask(cb) {
    if (typeof cb !== 'function') throw new TypeError('queueMicrotask: callback is not a function');
    Promise.resolve().then(() => cb());
});

/* ---------------- structuredClone ---------------- */

/* Values may come from another document of the page (postMessage between
 * frames): types are told by instanceof, then by the class tag, which
 * holds across realms, and the copy is always made of this realm's
 * objects. */
const classTag = (v) => Object.prototype.toString.call(v).slice(8, -1);
function structuredClone(value, options) {
    const seen = new Map();
    let budget = 1000000;
    const clone = (v) => {
        if (v === null || (typeof v !== 'object' && typeof v !== 'function')) {
            if (typeof v === 'symbol') throw domErr('A symbol could not be cloned', 'DataCloneError');
            return v;
        }
        if (typeof v === 'function') throw domErr('A function could not be cloned', 'DataCloneError');
        if (seen.has(v)) return seen.get(v);
        if (--budget < 0) throw domErr('The value is too large to clone', 'DataCloneError');
        let out;
        if (Array.isArray(v)) {
            out = new Array(v.length);
            seen.set(v, out);
            for (const k of Object.keys(v)) out[k] = clone(v[k]);
            return out;
        }
        const t = classTag(v);
        if (v instanceof Date || t === 'Date') out = new Date(Date.prototype.getTime.call(v));
        else if (v instanceof RegExp || t === 'RegExp') out = new RegExp(v.source, v.flags);
        else if (v instanceof Boolean || v instanceof Number || v instanceof String || t === 'Boolean' || t === 'Number' || t === 'String') out = Object(v.valueOf());
        else if (v instanceof ArrayBuffer || t === 'ArrayBuffer') out = new Uint8Array(new Uint8Array(v)).buffer;
        else if (ArrayBuffer.isView(v)) {
            const buf = clone(v.buffer);
            const C = G[t];
            if (t === 'DataView') out = new DataView(buf, v.byteOffset, v.byteLength);
            else if (typeof C === 'function' && /Array$/.test(t)) out = new C(buf, v.byteOffset, v.length);
            else throw domErr('The object could not be cloned', 'DataCloneError');
        } else if (v instanceof Map || t === 'Map') {
            out = new Map();
            seen.set(v, out);
            Map.prototype.forEach.call(v, (b, a) => out.set(clone(a), clone(b)));
            return out;
        } else if (v instanceof Set || t === 'Set') {
            out = new Set();
            seen.set(v, out);
            Set.prototype.forEach.call(v, (a) => out.add(clone(a)));
            return out;
        } else if (v instanceof Error || t === 'Error') {
            const C = { EvalError, RangeError, ReferenceError, SyntaxError, TypeError, URIError }[v.name] || Error;
            out = new C(v.message);
            if (v.stack !== undefined) out.stack = String(v.stack);
        } else if (v instanceof Blob) {
            out = v;
        } else if (isNode(v) || v instanceof EventTarget || v instanceof WeakMap || v instanceof WeakSet ||
            v instanceof Promise || (typeof WeakRef === 'function' && v instanceof WeakRef) ||
            t === 'WeakMap' || t === 'WeakSet' || t === 'Promise' || t === 'WeakRef' || t === 'Window' || t === 'MessagePort') {
            throw domErr('The object could not be cloned', 'DataCloneError');
        } else {
            out = {};
            seen.set(v, out);
            for (const k of Object.keys(v)) out[k] = clone(v[k]);
            return out;
        }
        seen.set(v, out);
        return out;
    };
    const out = clone(value);
    /* transferred buffers: the copy has the bytes, the original is emptied */
    if (options && typeof options === 'object' && options.transfer !== undefined && options.transfer !== null)
        for (const t of Array.from(options.transfer)) if (t instanceof ArrayBuffer || classTag(t) === 'ArrayBuffer') N.detach(t);
    return out;
}
defGlobal('structuredClone', function (value, options) {
    if (!arguments.length) throw new TypeError('structuredClone: 1 argument required');
    return structuredClone(value, options);
});

/* ---------------- messages between runtimes ----------------
 * A worker is another QuickJS runtime: no object passes between it and
 * the page. scEncode turns a value into a tree of arrays, primitives and
 * ArrayBuffers that C writes as bytes (worker.c); scDecode builds the
 * value again on the other side. Objects are ['tag', id, ...] with '@'
 * for a reference to one already seen; a transferred port is ['P', i],
 * the i-th of the channels that go with the message. */
function scEncode(value, portIndex) {
    const ids = new Map();
    let next = 0, budget = 1000000;
    const no = (what) => domErr(what + ' could not be cloned', 'DataCloneError');
    const enc = (v) => {
        if (v === null || (typeof v !== 'object' && typeof v !== 'function')) {
            if (typeof v === 'symbol') throw no('A symbol');
            return v;
        }
        if (typeof v === 'function') throw no('A function');
        if (ids.has(v)) return ['@', ids.get(v)];
        if (portIndex && portIndex.has(v)) return ['P', portIndex.get(v)];
        if (--budget < 0) throw domErr('The value is too large to clone', 'DataCloneError');
        const id = next++;
        ids.set(v, id);
        if (Array.isArray(v)) {
            const items = [];
            for (const k of Object.keys(v)) items.push(k, enc(v[k]));
            return ['A', id, v.length, items];
        }
        const t = classTag(v);
        if (t === 'Date') return ['D', id, Date.prototype.getTime.call(v)];
        if (t === 'RegExp') return ['R', id, v.source, v.flags];
        if (t === 'Boolean' || t === 'Number' || t === 'String') return ['B', id, v.valueOf()];
        if (t === 'ArrayBuffer') return ['Y', id, v];
        if (ArrayBuffer.isView(v)) return ['V', id, t, enc(v.buffer), v.byteOffset, t === 'DataView' ? v.byteLength : v.length];
        if (t === 'Map') { const items = []; Map.prototype.forEach.call(v, (b, a) => items.push(enc(a), enc(b))); return ['M', id, items]; }
        if (t === 'Set') { const items = []; Set.prototype.forEach.call(v, (a) => items.push(enc(a))); return ['S', id, items]; }
        if (v instanceof Error || t === 'Error') return ['E', id, String(v.name), String(v.message), v.stack === undefined ? null : String(v.stack)];
        if (v instanceof Blob) return ['L', id, v instanceof File ? 1 : 0, v._b, v.type, v instanceof File ? v.name : '', v instanceof File ? v.lastModified : 0];
        if (isNode(v) || v instanceof EventTarget || t === 'WeakMap' || t === 'WeakSet' || t === 'Promise' || t === 'WeakRef' ||
            t === 'Window' || t === 'MessagePort' || t === 'SharedArrayBuffer') throw no('The object');
        const items = [];
        for (const k of Object.keys(v)) items.push(k, enc(v[k]));
        return ['O', id, items];
    };
    return enc(value);
}
function scDecode(tree, ports) {
    const objs = new Map();
    const dec = (n) => {
        if (!Array.isArray(n)) return n;
        const id = n[1];
        let out;
        switch (n[0]) {
        case '@': return objs.get(id);
        case 'P': return ports ? ports[id] : null;
        case 'A': out = new Array(n[2]); objs.set(id, out); for (let i = 0; i < n[3].length; i += 2) out[n[3][i]] = dec(n[3][i + 1]); return out;
        case 'O': out = {}; objs.set(id, out); for (let i = 0; i < n[2].length; i += 2) out[n[2][i]] = dec(n[2][i + 1]); return out;
        case 'M': out = new Map(); objs.set(id, out); for (let i = 0; i < n[2].length; i += 2) out.set(dec(n[2][i]), dec(n[2][i + 1])); return out;
        case 'S': out = new Set(); objs.set(id, out); for (const x of n[2]) out.add(dec(x)); return out;
        case 'D': out = new Date(n[2]); break;
        case 'R': out = new RegExp(n[2], n[3]); break;
        case 'B': out = Object(n[2]); break;
        case 'Y': out = n[2]; break;
        case 'V': {
            const buf = dec(n[3]);
            const C = G[n[2]];
            out = n[2] === 'DataView' ? new DataView(buf, n[4], n[5]) : new C(buf, n[4], n[5]);
            break;
        }
        case 'E': {
            const C = { EvalError, RangeError, ReferenceError, SyntaxError, TypeError, URIError }[n[2]] || Error;
            out = new C(n[3]);
            if (n[4] !== null) out.stack = n[4];
            break;
        }
        case 'L':
            out = n[2] ? new File([], n[5], { type: n[4], lastModified: n[6] }) : new Blob([], { type: n[4] });
            out._b = n[3];
            break;
        default: throw domErr('The message could not be read', 'DataCloneError');
        }
        objs.set(id, out);
        return out;
    };
    return dec(tree);
}
/* Prepares a message for another runtime: [tree, channel numbers]. The
 * ports of the transfer list travel as channels (exportEnds); its
 * buffers are emptied once written. */
function encodeMessage(data, transfer) {
    const list = transferList(transfer);
    const index = new Map();
    let i = 0;
    for (const p of list) if (portEnds.has(p)) index.set(p, i++);
    const tree = scEncode(data, index);
    const ends = transferOf(transfer);
    return [tree, exportEnds(ends), ends.buffers];
}
const detachAll = (buffers) => { if (buffers) for (const b of buffers) N.detach(b); };

/* ---------------- MessageChannel ----------------
 * A port is one end of a pipe. The end's state (its queue and its peer)
 * is a record kept in portEnds, where page script cannot reach it. A
 * message is cloned when posted and again by the receiving port, in its
 * own realm, and delivered as a task once the port has started (start(),
 * or setting onmessage). Transferring a port (the transfer list of
 * postMessage, to a window or a port) moves its end's record to a new
 * port in the receiving document and leaves the old port closed; the
 * record is never a value page script sees, so the two documents share
 * no object. */
const portEnds = new WeakMap();
const newEnd = () => ({ queue: [], peer: null, wake: null, closed: false });
/* Ends whose other end is in another runtime, by channel number. Such an
 * end's peer is { remote: channel }. */
const remoteEnds = new Map();
const transferList = (opts, list) => {
    if (opts && typeof opts === 'object' && !Array.isArray(opts) && opts.transfer !== undefined) list = opts.transfer;
    else if (Array.isArray(opts)) list = opts;
    return list === undefined || list === null ? [] : Array.from(list);
};
/* The ends of the ports in a transfer list (detached from their ports).
 * The list's ArrayBuffers are returned as the result's buffers, for the
 * caller to empty once the message is cloned. */
function transferOf(opts, list) {
    const items = transferList(opts, list);
    const ends = [];
    ends.buffers = [];
    const ports = [];
    for (const p of items) {
        if (p instanceof ArrayBuffer || (p !== null && typeof p === 'object' && classTag(p) === 'ArrayBuffer')) {
            if (ends.buffers.includes(p)) throw domErr('The transfer list holds a buffer twice', 'DataCloneError');
            ends.buffers.push(p);
            continue;
        }
        const e = p !== null && typeof p === 'object' ? portEnds.get(p) : undefined;
        if (!e || ends.includes(e)) throw domErr('The transfer list holds a value that cannot be transferred', 'DataCloneError');
        ends.push(e);
        ports.push(p);
    }
    for (const p of ports) {
        const e = portEnds.get(p);
        e.wake = null; e.onclose = null;
        portEnds.delete(p);
        /* an end of a channel to another runtime leaves this one */
        if (e.peer && e.peer.remote !== undefined) { remoteEnds.delete(e.peer.remote); N.portRelease(e.peer.remote); }
    }
    return ends;
}
P.transferOf = transferOf;
/* Ends that go to another runtime, as channel numbers. An end whose peer
 * stays here gets a new channel; one whose peer is already elsewhere
 * keeps its channel. Messages that waited at the end follow it. */
function exportEnds(ends) {
    const cids = [];
    for (const e of ends) {
        let cid;
        if (e.peer && e.peer.remote !== undefined) cid = e.peer.remote;
        else {
            cid = N.portNew();
            const p = e.peer;
            if (p && !p.closed) { p.peer = { remote: cid }; remoteEnds.set(cid, p); N.portHold(cid); }
        }
        for (const m of e.queue) {
            const index = new Map();
            N.portForward(cid, scEncode(m.data, index), exportEnds(m.ends || []));
        }
        e.queue = [];
        e.closed = true;
        cids.push(cid);
    }
    return cids;
}
/* Ports of this realm for ends that arrived with a message: end records
 * (from a document of this runtime) or channel numbers (from another
 * runtime). */
const adoptEnds = (ends) => {
    const out = [];
    if (ends) for (let i = 0; i < ends.length; i++) {
        let e = ends[i];
        if (typeof e === 'number') { const cid = e; e = newEnd(); e.peer = { remote: cid }; }
        if (e.peer && e.peer.remote !== undefined) { remoteEnds.set(e.peer.remote, e); N.portHold(e.peer.remote); }
        out.push(new MessagePort(S.owner, e));
    }
    return Object.freeze(out);
};
/* A message for the end of a channel held here (from worker.c). */
P.portDeliver = function (cid, tree, cids) {
    const e = remoteEnds.get(cid);
    if (!e || e.closed) return;
    e.queue.push({ tree, ends: cids });
    if (e.wake) e.wake();
};
class MessagePort extends EventTarget {
    constructor(key, end) {
        if (key !== S.owner) throw new TypeError('Illegal constructor');
        super();
        this._started = false;
        portEnds.set(this, end);
        end.wake = () => { if (this._started) this._drain(); };
        end.onclose = () => N.queueTask(() => fire(this, new Event('close')));
    }
    postMessage(data, transfer) {
        if (!arguments.length) throw new TypeError('postMessage: 1 argument required');
        const e = portEnds.get(this);
        if (e && !e.closed && e.peer && e.peer.remote !== undefined) {
            /* the other end is in another runtime */
            const [tree, cids, buffers] = encodeMessage(data, transfer);
            N.portPost(e.peer.remote, tree, cids);
            detachAll(buffers);
            return;
        }
        const ends = transferOf(transfer);
        if (e && ends.includes(e.peer)) throw domErr('A port cannot be sent through itself', 'DataCloneError');
        const to = e && !e.closed ? e.peer : null;
        const msg = { data: structuredClone(data), ends };
        detachAll(ends.buffers);
        if (!to || to.closed) return;
        to.queue.push(msg);
        if (to.wake) to.wake();
    }
    _drain() {
        N.queueTask(() => {
            const e = portEnds.get(this);
            while (e && this._started && !e.closed && e.queue.length) {
                const m = e.queue.shift();
                let data;
                const ports = adoptEnds(m.ends);
                try { data = m.tree !== undefined ? scDecode(m.tree, ports) : structuredClone(m.data); }
                catch (err) { fire(this, new MessageEvent('messageerror', {})); continue; }
                fire(this, new MessageEvent('message', { data, ports }));
            }
        });
    }
    start() {
        if (this._started) return;
        this._started = true;
        const e = portEnds.get(this);
        if (e && e.queue.length) this._drain();
    }
    close() {
        const e = portEnds.get(this);
        if (!e || e.closed) return;
        e.closed = true;
        const o = e.peer;
        e.peer = null;
        if (o && o.remote !== undefined) { remoteEnds.delete(o.remote); N.portRelease(o.remote); return; }
        if (o && !o.closed) { o.peer = null; if (o.onclose) o.onclose(); }
    }
    get onmessage() { return this._onmessage || null; }
    set onmessage(fn) {
        if (this._onmessage) this.removeEventListener('message', this._onmessage);
        this._onmessage = typeof fn === 'function' ? fn : null;
        if (this._onmessage) this.addEventListener('message', this._onmessage);
        this.start();
    }
}
defineHandlers(MessagePort.prototype, ['messageerror', 'close']);
class MessageChannel {
    constructor() {
        const ea = newEnd(), eb = newEnd();
        ea.peer = eb; eb.peer = ea;
        const a = new MessagePort(S.owner, ea), b = new MessagePort(S.owner, eb);
        Object.defineProperties(this, { port1: { value: a, enumerable: true }, port2: { value: b, enumerable: true } });
    }
}

/* ---------------- crypto ----------------
 * Present only when the embedder has a strong random source
 * (AuJsHooks.random). */
if (N.random(0) !== null) {
    class Crypto {
        constructor() { throw new TypeError('Illegal constructor'); }
        getRandomValues(a) {
            if (!ArrayBuffer.isView(a) || a instanceof DataView || a instanceof Float32Array || a instanceof Float64Array ||
                (typeof Float16Array === 'function' && a instanceof Float16Array))
                throw domErr('getRandomValues: the argument is not an integer typed array', 'TypeMismatchError');
            if (a.byteLength > 65536) throw domErr('getRandomValues: more than 65536 bytes', 'QuotaExceededError');
            const b = N.random(a.byteLength);
            if (!b) throw domErr('The random source is not ready', 'OperationError');
            new Uint8Array(a.buffer, a.byteOffset, a.byteLength).set(new Uint8Array(b));
            return a;
        }
        randomUUID() {
            const b = new Uint8Array(16);
            this.getRandomValues(b);
            b[6] = (b[6] & 0x0f) | 0x40;
            b[8] = (b[8] & 0x3f) | 0x80;
            const h = Array.from(b, (x) => (x < 16 ? '0' : '') + x.toString(16)).join('');
            return h.slice(0, 8) + '-' + h.slice(8, 12) + '-' + h.slice(12, 16) + '-' + h.slice(16, 20) + '-' + h.slice(20);
        }
    }
    /* SubtleCrypto: digest (SHA-1, SHA-256, SHA-384, SHA-512) and HMAC
     * with raw keys (importKey, sign, verify, generateKey, exportKey).
     * Everything else rejects with NotSupportedError. */
    const algName = (a) => str(a && typeof a === 'object' ? a.name : a);
    const hashOf = (a) => {
        const n = algName(a).toUpperCase();
        if (!['SHA-1', 'SHA-256', 'SHA-384', 'SHA-512'].includes(n)) throw domErr('Unrecognized hash: ' + algName(a), 'NotSupportedError');
        return n;
    };
    const srcBytes = (d) => {
        if (d instanceof ArrayBuffer) return new Uint8Array(d.slice(0));
        if (ArrayBuffer.isView(d)) return new Uint8Array(d.buffer.slice(d.byteOffset, d.byteOffset + d.byteLength));
        throw new TypeError('The data is not a BufferSource');
    };
    const unsupported = () => Promise.reject(domErr('The algorithm or operation is not supported', 'NotSupportedError'));
    const keyData = new WeakMap();
    class CryptoKey {
        constructor() { throw new TypeError('Illegal constructor'); }
        get type() { return 'secret'; }
        get extractable() { return keyData.get(this).extractable; }
        get algorithm() { const k = keyData.get(this); return { name: 'HMAC', hash: { name: k.hash }, length: k.bytes.length * 8 }; }
        get usages() { return keyData.get(this).usages.slice(); }
    }
    const makeKey = (bytes, hash, extractable, usages) => {
        const k = Object.create(CryptoKey.prototype);
        keyData.set(k, { bytes, hash, extractable: !!extractable, usages: Array.from(usages || []).map(str) });
        return k;
    };
    const attempt = (fn) => { try { return Promise.resolve(fn()); } catch (e) { return Promise.reject(e); } };
    class SubtleCrypto {
        constructor() { throw new TypeError('Illegal constructor'); }
        digest(alg, data) { return attempt(() => N.digest(hashOf(alg), srcBytes(data))); }
        importKey(format, data, alg, extractable, usages) {
            return attempt(() => {
                if (algName(alg).toUpperCase() !== 'HMAC' || str(format) !== 'raw') throw domErr('Only raw HMAC keys are supported', 'NotSupportedError');
                return makeKey(srcBytes(data), hashOf(alg.hash), extractable, usages);
            });
        }
        generateKey(alg, extractable, usages) {
            return attempt(() => {
                if (algName(alg).toUpperCase() !== 'HMAC') throw domErr('Only HMAC keys are supported', 'NotSupportedError');
                const hash = hashOf(alg.hash);
                const bytes = new Uint8Array(alg.length ? alg.length >>> 3 : hash === 'SHA-384' || hash === 'SHA-512' ? 128 : 64);
                G.crypto.getRandomValues(bytes);
                return makeKey(bytes, hash, extractable, usages);
            });
        }
        exportKey(format, key) {
            return attempt(() => {
                const k = keyData.get(key);
                if (!k) throw new TypeError('The key is not a CryptoKey');
                if (str(format) !== 'raw') throw domErr('Only the raw format is supported', 'NotSupportedError');
                if (!k.extractable) throw domErr('The key is not extractable', 'InvalidAccessError');
                return k.bytes.slice().buffer;
            });
        }
        sign(alg, key, data) {
            return attempt(() => {
                const k = keyData.get(key);
                if (!k) throw new TypeError('The key is not a CryptoKey');
                if (algName(alg).toUpperCase() !== 'HMAC') throw domErr('Only HMAC is supported', 'NotSupportedError');
                if (!k.usages.includes('sign')) throw domErr('The key may not sign', 'InvalidAccessError');
                return N.hmac(k.hash, k.bytes, srcBytes(data));
            });
        }
        verify(alg, key, signature, data) {
            return attempt(() => {
                const k = keyData.get(key);
                if (!k) throw new TypeError('The key is not a CryptoKey');
                if (algName(alg).toUpperCase() !== 'HMAC') throw domErr('Only HMAC is supported', 'NotSupportedError');
                if (!k.usages.includes('verify')) throw domErr('The key may not verify', 'InvalidAccessError');
                const a = new Uint8Array(N.hmac(k.hash, k.bytes, srcBytes(data))), b = srcBytes(signature);
                let diff = a.length ^ b.length;
                for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i % (b.length || 1)];
                return diff === 0;
            });
        }
        encrypt() { return unsupported(); } decrypt() { return unsupported(); }
        deriveBits() { return unsupported(); } deriveKey() { return unsupported(); }
        wrapKey() { return unsupported(); } unwrapKey() { return unsupported(); }
    }
    const subtle = Object.create(SubtleCrypto.prototype);
    defProps(Crypto.prototype, { get subtle() { return subtle; } });
    tag(Crypto); tag(SubtleCrypto); tag(CryptoKey);
    defGlobal('Crypto', Crypto);
    defGlobal('SubtleCrypto', SubtleCrypto);
    defGlobal('CryptoKey', CryptoKey);
    defGlobal('crypto', Object.create(Crypto.prototype));
}
tag(MessagePort); tag(MessageChannel);
defGlobal('MessagePort', MessagePort);
defGlobal('MessageChannel', MessageChannel);

/* ---------------- URL, URLSearchParams ---------------- */

/* The parser is N.urlParse (src/js/url.c, the WHATWG parser of src/url):
 * [href, protocol, username, password, hostname, port, pathname, search,
 * hash] or null. */
const specialPorts = { 'http:': '80', 'https:': '443', 'ws:': '80', 'wss:': '443', 'ftp:': '21' };
function parseUrl(input, base) {
    const r = base === undefined ? N.urlParse(input) : N.urlParse(input, base);
    if (!r) throw new TypeError('Invalid URL: ' + input);
    return r;
}
class URL {
    constructor(url, base) {
        url = str(url);
        if (base !== undefined) base = str(base instanceof URL ? base.href : base);
        if (base !== undefined) parseUrl(base);
        this._p = parseUrl(url, base);
        this._sp = null;
    }
    static canParse(url, base) { try { new URL(url, base); return true; } catch (e) { return false; } }
    static parse(url, base) { try { return new URL(url, base); } catch (e) { return null; } }
    static createObjectURL(blob) { return blobUrls.create(blob); }
    static revokeObjectURL(u) { blobUrls.revoke(str(u)); }
    _set(p) { this._p = p; if (this._sp) this._sp._load(this._p[7]); }
    _rebuild(parts) {
        const [, protocol, username, password, hostname, port, pathname, search, hash] = parts;
        let s = protocol;
        const hasHost = hostname !== '' || protocol === 'file:' || specialPorts[protocol] !== undefined;
        if (hasHost) {
            s += '//';
            if (username || password) s += username + (password ? ':' + password : '') + '@';
            s += hostname + (port ? ':' + port : '');
        }
        s += pathname + search + hash;
        const r = N.urlParse(s);
        if (r) this._set(r);
    }
    get href() { return this._p[0]; }
    set href(v) { this._set(parseUrl(str(v))); }
    toString() { return this._p[0]; }
    toJSON() { return this._p[0]; }
    get origin() { const o = N.originOf(this._p[0]); return o === 'file://' ? 'null' : o; }
    get protocol() { return this._p[1]; }
    set protocol(v) {
        v = str(v).replace(/:.*$/, '').toLowerCase();
        if (!/^[a-z][a-z0-9+.-]*$/.test(v)) return;
        const special = specialPorts[this._p[1]] !== undefined || this._p[1] === 'file:';
        const toSpecial = specialPorts[v + ':'] !== undefined || v === 'file';
        if (special !== toSpecial) return;
        const p = this._p.slice(); p[1] = v + ':';
        if (p[5] === specialPorts[p[1]]) p[5] = '';
        this._rebuild(p);
    }
    get username() { return this._p[2]; }
    set username(v) { if (!this._p[4] || this._p[1] === 'file:') return; const p = this._p.slice(); p[2] = encodeURIComponent(str(v)); this._rebuild(p); }
    get password() { return this._p[3]; }
    set password(v) { if (!this._p[4] || this._p[1] === 'file:') return; const p = this._p.slice(); p[3] = encodeURIComponent(str(v)); this._rebuild(p); }
    get host() { return this._p[4] + (this._p[5] ? ':' + this._p[5] : ''); }
    set host(v) {
        const m = /^([^:/?#]*)(?::(\d*))?/.exec(str(v));
        if (!m || !m[1]) return;
        const p = this._p.slice(); p[4] = m[1]; if (m[2] !== undefined) p[5] = m[2] === specialPorts[p[1]] ? '' : m[2];
        this._rebuild(p);
    }
    get hostname() { return this._p[4]; }
    set hostname(v) { const h = /^[^:/?#]*/.exec(str(v))[0]; if (!h) return; const p = this._p.slice(); p[4] = h; this._rebuild(p); }
    get port() { return this._p[5]; }
    set port(v) {
        if (!this._p[4] || this._p[1] === 'file:') return;
        const d = /^\d*/.exec(str(v))[0];
        if (d === '' && str(v) !== '') return;
        const p = this._p.slice(); p[5] = d === '' || d === specialPorts[p[1]] ? '' : String(parseInt(d, 10));
        if (+p[5] > 65535) return;
        this._rebuild(p);
    }
    get pathname() { return this._p[6]; }
    set pathname(v) {
        v = str(v);
        const p = this._p.slice();
        p[6] = specialPorts[p[1]] !== undefined || p[1] === 'file:' ? (v.startsWith('/') ? v : '/' + v) : v;
        p[6] = p[6].replace(/[?#]/g, (c) => (c === '?' ? '%3F' : '%23'));
        this._rebuild(p);
    }
    get search() { return this._p[7] === '?' ? '' : this._p[7]; }
    set search(v) {
        v = str(v);
        const p = this._p.slice();
        p[7] = v === '' ? '' : '?' + v.replace(/^\?/, '').replace(/#/g, '%23');
        this._rebuild(p);
    }
    get searchParams() {
        if (!this._sp) { this._sp = new URLSearchParams(this.search); this._sp._url = this; }
        return this._sp;
    }
    get hash() { return this._p[8] === '#' ? '' : this._p[8]; }
    set hash(v) { v = str(v); const p = this._p.slice(); p[8] = v === '' ? '' : '#' + v.replace(/^#/, ''); this._rebuild(p); }
}
tag(URL);
defGlobal('URL', URL);
defGlobal('webkitURL', URL);

const formDecode = (s) => {
    s = s.replace(/\+/g, ' ');
    try { return decodeURIComponent(s); } catch (e) {
        return s.replace(/%([0-9a-fA-F]{2})/g, (m, h) => String.fromCharCode(parseInt(h, 16)));
    }
};
class URLSearchParams {
    constructor(init = '') {
        this._l = [];
        this._url = null;
        if (init instanceof URLSearchParams) this._l = init._l.map((p) => p.slice());
        else if (init !== null && typeof init === 'object') {
            if (typeof init[Symbol.iterator] === 'function') {
                for (const pair of init) {
                    const a = Array.from(pair);
                    if (a.length !== 2) throw new TypeError('URLSearchParams: each pair must have two items');
                    this._l.push([str(a[0]).toWellFormed(), str(a[1]).toWellFormed()]);
                }
            } else for (const k of Object.keys(init)) this._l.push([k.toWellFormed(), str(init[k]).toWellFormed()]);
        } else this._load(str(init));
    }
    _load(s) {
        this._l = [];
        s = s.replace(/^\?/, '');
        for (const part of s.split('&')) {
            if (!part) continue;
            const i = part.indexOf('=');
            this._l.push(i < 0 ? [formDecode(part), ''] : [formDecode(part.slice(0, i)), formDecode(part.slice(i + 1))]);
        }
    }
    _update() {
        if (!this._url) return;
        const s = this.toString();
        const u = this._url;
        const p = u._p.slice();
        p[7] = s ? '?' + s : '';
        const sp = u._sp; u._sp = null;
        u._rebuild(p);
        u._sp = sp;
    }
    get size() { return this._l.length; }
    append(k, v) { this._l.push([str(k), str(v)]); this._update(); }
    delete(k, v) { k = str(k); this._l = this._l.filter((p) => p[0] !== k || (v !== undefined && p[1] !== str(v))); this._update(); }
    get(k) { k = str(k); const p = this._l.find((x) => x[0] === k); return p ? p[1] : null; }
    getAll(k) { k = str(k); return this._l.filter((x) => x[0] === k).map((x) => x[1]); }
    has(k, v) { k = str(k); return this._l.some((x) => x[0] === k && (v === undefined || x[1] === str(v))); }
    set(k, v) {
        k = str(k); v = str(v);
        const i = this._l.findIndex((x) => x[0] === k);
        if (i < 0) this._l.push([k, v]);
        else { this._l[i][1] = v; this._l = this._l.filter((x, j) => j <= i || x[0] !== k); }
        this._update();
    }
    sort() {
        const cmp = (a, b) => { for (let i = 0; i < Math.min(a.length, b.length); i++) { const d = a.charCodeAt(i) - b.charCodeAt(i); if (d) return d; } return a.length - b.length; };
        this._l = this._l.map((p, i) => [p, i]).sort((x, y) => cmp(x[0][0], y[0][0]) || x[1] - y[1]).map((x) => x[0]);
        this._update();
    }
    forEach(fn, thisArg) { for (const [k, v] of this._l.slice()) fn.call(thisArg, v, k, this); }
    keys() { return this._l.map((p) => p[0])[Symbol.iterator](); }
    values() { return this._l.map((p) => p[1])[Symbol.iterator](); }
    entries() { return this._l.map((p) => p.slice())[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    toString() { return formEncode(this._l); }
}
tag(URLSearchParams);
defGlobal('URLSearchParams', URLSearchParams);

/* ---------------- TextEncoder, TextDecoder ---------------- */

class TextEncoder {
    get encoding() { return 'utf-8'; }
    encode(s = '') { return N.utf8Encode(str(s).toWellFormed()); }
    encodeInto(s, dest) {
        if (!(dest instanceof Uint8Array)) throw new TypeError('encodeInto: the destination is not a Uint8Array');
        s = str(s).toWellFormed();
        let read = 0, written = 0;
        for (const ch of s) {
            const b = N.utf8Encode(ch);
            if (written + b.length > dest.length) break;
            dest.set(b, written);
            written += b.length;
            read += ch.length;
        }
        return { read, written };
    }
}
tag(TextEncoder);
defGlobal('TextEncoder', TextEncoder);
const utf8Labels = ['unicode-1-1-utf-8', 'unicode11utf8', 'unicode20utf8', 'utf-8', 'utf8', 'x-unicode20utf8'];
const latin1Labels = ['ansi_x3.4-1968', 'ascii', 'cp1252', 'cp819', 'csisolatin1', 'ibm819', 'iso-8859-1', 'iso-ir-100',
    'iso8859-1', 'iso88591', 'iso_8859-1', 'iso_8859-1:1987', 'l1', 'latin1', 'us-ascii', 'windows-1252', 'x-cp1252'];
const cp1252 = '€\x81‚ƒ„…†‡ˆ‰Š‹Œ\x8dŽ\x8f\x90‘’“”•–—˜™š›œ\x9džŸ';
class TextDecoder {
    constructor(label = 'utf-8', opts = {}) {
        const l = str(label).trim().toLowerCase();
        if (utf8Labels.includes(l)) this._enc = 'utf-8';
        else if (latin1Labels.includes(l)) this._enc = 'windows-1252';
        else if (l === 'utf-16le' || l === 'utf-16') this._enc = 'utf-16le';
        else if (l === 'utf-16be') this._enc = 'utf-16be';
        else throw new RangeError('The encoding label is not supported: ' + label);
        this._fatal = !!(opts && opts.fatal);
        this._ignoreBOM = !!(opts && opts.ignoreBOM);
        this._pending = null;
        this._bomSeen = false;
    }
    get encoding() { return this._enc; }
    get fatal() { return this._fatal; }
    get ignoreBOM() { return this._ignoreBOM; }
    decode(input, opts) {
        let bytes;
        if (input === undefined || input === null) bytes = new Uint8Array(0);
        else if (input instanceof ArrayBuffer) bytes = new Uint8Array(input);
        else if (ArrayBuffer.isView(input)) bytes = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
        else throw new TypeError('decode: the input is not a BufferSource');
        const stream = !!(opts && opts.stream);
        if (this._pending) { const b = new Uint8Array(this._pending.length + bytes.length); b.set(this._pending); b.set(bytes, this._pending.length); bytes = b; this._pending = null; }
        if (this._enc === 'utf-8') {
            if (stream) {
                /* keep an incomplete trailing sequence for the next call */
                let cut = bytes.length;
                for (let i = Math.max(0, bytes.length - 3); i < bytes.length; i++) {
                    const b = bytes[i];
                    /* only a byte that can lead a sequence waits (0xff never does) */
                    const need = b >= 0xf5 ? 0 : b >= 0xf0 ? 4 : b >= 0xe0 ? 3 : b >= 0xc2 ? 2 : 0;
                    if (need && i + need > bytes.length) { cut = i; break; }
                }
                if (cut < bytes.length) { this._pending = bytes.slice(cut); bytes = bytes.subarray(0, cut); }
            }
            const skipBom = this._ignoreBOM || this._bomSeen;
            const s = N.utf8Decode(bytes, this._fatal, skipBom);
            if (bytes.length) this._bomSeen = stream;
            if (!stream) this._bomSeen = false;
            return s;
        }
        if (this._enc === 'windows-1252') {
            let s = '';
            for (const b of bytes) s += b >= 0x80 && b < 0xa0 ? cp1252[b - 0x80] : String.fromCharCode(b);
            return s;
        }
        const le = this._enc === 'utf-16le';
        let n = bytes.length & ~1;
        if (stream && bytes.length & 1) this._pending = bytes.slice(n);
        let s = '';
        let i = 0;
        if (!this._ignoreBOM && n >= 2 && ((le && bytes[0] === 0xff && bytes[1] === 0xfe) || (!le && bytes[0] === 0xfe && bytes[1] === 0xff))) i = 2;
        for (; i < n; i += 2) s += String.fromCharCode(le ? bytes[i] | (bytes[i + 1] << 8) : (bytes[i] << 8) | bytes[i + 1]);
        if (!stream && bytes.length & 1) { if (this._fatal) throw new TypeError('The data is not valid UTF-16'); s += '�'; }
        return this._fatal || s.isWellFormed() ? s : s.toWellFormed();
    }
}
tag(TextDecoder);
defGlobal('TextDecoder', TextDecoder);

/* ---------------- modules ---------------- */

function fetchModules(urls) {
    return Promise.all(urls.map((u) => new Promise((resolve) => {
        P.fetchText(u, 2, (body) => {
            if (body === null) { resolve(u); return; }
            N.moduleStore(u, body);
            resolve(null);
        });
    }))).then((failed) => failed.filter(Boolean));
}
/* Loads, links and evaluates a module script. Resolves to true on
 * success. Errors are reported. */
async function runModuleScript(url, src, el) {
    try {
        for (let round = 0; round < 200; round++) {
            const r = N.moduleEval(url, src, src !== null);
            if (Array.isArray(r)) {
                const failed = await fetchModules(r);
                if (failed.length) {
                    N.print(3, 'Failed to load module script: ' + failed.join(', '));
                    if (el) fireSimple(el, 'error');
                    return false;
                }
                continue;
            }
            await r.promise;
            return true;
        }
        N.print(3, 'Module graph too large: ' + url);
        return false;
    } catch (e) {
        reportError(e);
        return false;
    }
}
P.runModule = function (url, src, el, id) {
    runModuleScript(url, src, el).then((ok) => N.moduleFinished(id, ok));
};
P.dynamicImport = function (base, spec, token) {
    (async () => {
        try {
            for (let round = 0; round < 200; round++) {
                const r = N.moduleLoad(base, spec, token);
                if (!Array.isArray(r)) return;
                const failed = await fetchModules(r);
                if (failed.length) throw new TypeError('Failed to fetch dynamically imported module: ' + failed.join(', '));
            }
            throw new TypeError('Module graph too large: ' + spec);
        } catch (e) {
            N.moduleFail(token, e);
        }
    })();
};
P.addImportMap = function (text) {
    let m;
    try { m = JSON.parse(text); } catch (e) { reportError(new SyntaxError('Invalid import map: ' + e.message)); return; }
    if (!m || typeof m !== 'object' || Array.isArray(m)) { reportError(new TypeError('Invalid import map')); return; }
    const pairs = [];
    const imports = m.imports && typeof m.imports === 'object' ? m.imports : {};
    for (const k of Object.keys(imports)) {
        const v = imports[k];
        if (typeof v !== 'string') { N.print(2, 'Import map entry "' + k + '" is not a string; ignored.'); continue; }
        let target;
        try { target = new URL(v, N.docUrl(1)).href; } catch (e) { N.print(2, 'Import map entry "' + k + '" is not a URL; ignored.'); continue; }
        let key = k;
        if (/^(\/|\.\/|\.\.\/)/.test(k)) { try { key = new URL(k, N.docUrl(1)).href; } catch (e) { continue; } }
        if (key.endsWith('/') && !target.endsWith('/')) { N.print(2, 'Import map entry "' + k + '" must map to a URL ending in /.'); continue; }
        pairs.push([key, target]);
    }
    if (m.scopes) N.print(2, 'Import map scopes are not supported; ignored.');
    N.setImportMap(pairs);
};

/* ---------------- form encoding ---------------- */

const formEncode = (pairs) => pairs.map(([k, v]) => encodeForm(k) + '=' + encodeForm(v)).join('&');
function encodeForm(s) {
    s = str(s).replace(/\r?\n|\r/g, '\r\n');
    let out = '';
    for (const b of N.utf8Encode(s.toWellFormed())) {
        const c = String.fromCharCode(b);
        if (/[A-Za-z0-9*\-._]/.test(c)) out += c;
        else if (c === ' ') out += '+';
        else out += '%' + (b < 16 ? '0' : '') + b.toString(16).toUpperCase();
    }
    return out;
}

